#ifndef DFTRACER_UTILS_DATAFRAME_ABI_H
#define DFTRACER_UTILS_DATAFRAME_ABI_H

#include <dftracer/utils/core/common/abi.h>
#include <dftracer/utils/core/common/export.h>
#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/query/abi.h>
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
typedef enum {
    DFTU_SCALAR_TAG_I64 = 0,
    DFTU_SCALAR_TAG_U64 = 1,
    DFTU_SCALAR_TAG_F64 = 2,
    DFTU_SCALAR_TAG_STR = 3
} dftu_scalar_tag;

/** A scalar operand, tagged by its domain so a kernel converts it to the
 * column's element type. Pass a signed integer as I64, an unsigned integer as
 * U64, a float as F64, a string as STR (see dftu_scalar_tag).
 *
 * A STR scalar BORROWS `value.s` for the duration of the call: nothing stores a
 * dftu_scalar past the call it was passed to, and the owner of the text (an
 * ExprNode, a caller's std::string) outlives it. `len` sits in the padding
 * after `kind`, so the struct is 16 bytes and 8-aligned exactly as before and
 * no by-value call site changes. */
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
    } value;
};

/** Element type of a dftu_series (mirrors dataframe::TypeId ordinals). */
typedef enum {
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
} dftu_dtype;

/** Second/Milli/Micro/Nano granularity (mirrors dataframe::TimeUnit), for
 * Timestamp, Time32, Time64, and Duration columns. */
typedef enum {
    DFTU_TIME_UNIT_SECOND = 0,
    DFTU_TIME_UNIT_MILLI = 1,
    DFTU_TIME_UNIT_MICRO = 2,
    DFTU_TIME_UNIT_NANO = 3
} dftu_time_unit;

/** Create a FLAT column copying `n` values of `type` from `data`. `validity` is
 * an Arrow-layout bitmap (1 = valid) or NULL for no nulls. */
DFTU_EXPORT dftu_series* dftu_series_new_flat(dftu_dtype type, const void* data,
                                              int64_t n,
                                              const uint8_t* validity);

/** Create a FLAT column BORROWING `n` values of `type` from `data` (no copy):
 * the column's data buffer points straight at `data`. `data` must stay valid
 * until `release(release_ctx)` runs, which happens when the column's last
 * reference is dropped; pass NULL for `release` to borrow non-owned static
 * memory. `validity` is copied if non-NULL (usually NULL). Returns NULL on a
 * variable-width type (String/Binary), where borrowing a single flat buffer is
 * not meaningful. */
DFTU_EXPORT dftu_series* dftu_series_new_flat_borrowed(
    dftu_dtype type, const void* data, int64_t n, const uint8_t* validity,
    void (*release)(void* ctx), void* release_ctx);

/** Create a FLAT variable-width column (String or Binary) copying `n` values.
 * `offsets` has n+1 int32 entries into `data`; `data` holds offsets[n] bytes.
 * `validity` is an Arrow-layout bitmap (1 = valid) or NULL for no nulls. */
DFTU_EXPORT dftu_series* dftu_series_new_string(dftu_dtype type,
                                                const int32_t* offsets,
                                                const void* data, int64_t n,
                                                const uint8_t* validity);

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
DFTU_EXPORT int32_t dftu_series_encoding(const dftu_series* col);
DFTU_EXPORT int64_t dftu_series_length(const dftu_series* col);
DFTU_EXPORT int64_t dftu_series_null_count(const dftu_series* col);

/** Whether row `i` is null (0/1). A column with no validity bitmap is
 * all-valid.
 */
DFTU_EXPORT int32_t dftu_series_is_null(const dftu_series* col, int64_t i);

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

/** A new owned column sharing `col`'s buffers zero-copy (a reference-count
 * bump, no data copy). NULL if `col` is NULL. Underpins projection/rename frame
 * ops.
 */
DFTU_EXPORT dftu_series* dftu_series_share(const dftu_series* col);

/** A zero-copy view of the rows [offset, offset+len) of `col`: for a FLAT
 * column the result's data pointer is shifted into `col`'s buffer and keeps
 * it alive, so SIMD kernels run on the sub-range with no gather; a SELECTION
 * view slices its index buffer and stays a view over the base; any other view
 * is materialized first. Fixed-width only (the chunked columnar evaluator's
 * inputs); NULL otherwise. */
DFTU_EXPORT dftu_series* dftu_series_slice(const dftu_series* col,
                                           int64_t offset, int64_t len);

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
 * floors toward minus infinity, the remainder takes the divisor's sign, a
 * zero divisor is null, a negative integer exponent is null. Int64 when both
 * operands are integral (Bool included), else Float64. The scalar forms take
 * an I64 or F64 scalar; `reverse` nonzero computes `s <op> a`. */
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
typedef enum {
    DFTU_CMP_GT = 0,
    DFTU_CMP_GE = 1,
    DFTU_CMP_LT = 2,
    DFTU_CMP_LE = 3,
    DFTU_CMP_EQ = 4,
    DFTU_CMP_NE = 5
} dftu_cmp_op;

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

/** Convert a FLAT numeric column to the numeric type `target`. */
DFTU_EXPORT dftu_series* dftu_series_cast(const dftu_series* v,
                                          dftu_dtype target);

/** Unary numeric primitives for dftu_series_prim (mirrors dataframe::PrimOp).
 */
typedef enum {
    DFTU_PRIM_ILOG2 = 0,
    DFTU_PRIM_BIT_WIDTH = 1,
    DFTU_PRIM_POPCOUNT = 2,
    DFTU_PRIM_CLZ = 3,
    DFTU_PRIM_CTZ = 4,
    DFTU_PRIM_MIX64 = 5
} dftu_prim_op;

/** Unary numeric primitive over a FLAT Int64/Uint64 column, producing an Int64
 * column (`op` is a dftu_prim_op). Null if the input is not a 64-bit integer
 * column. */
DFTU_EXPORT dftu_series* dftu_series_prim(const dftu_series* v,
                                          dftu_prim_op op);

/** Boolean ops on bit-packed Bool columns for dftu_series_logical. */
typedef enum { DFTU_LOGICAL_AND = 0, DFTU_LOGICAL_OR = 1 } dftu_logical_op;
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
/** The capture `group` (0 = the whole match) of the first ECMAScript regex
 * `pattern` match in each row, as a String column; null where the row is null
 * or does not match (pandas `str.extract`). NULL if the pattern fails to
 * compile or `group` is negative. */
DFTU_EXPORT dftu_series* dftu_series_str_extract(const dftu_series* v,
                                                 const char* pattern,
                                                 int32_t pattern_len,
                                                 int64_t group);
/** True where the WHOLE string matches the ECMAScript regex `pattern` (the
 * regex entry point; std::regex). NULL if the pattern fails to compile. */
DFTU_EXPORT dftu_series* dftu_series_str_matches(const dftu_series* v,
                                                 const char* pattern,
                                                 int32_t pattern_len);
/** True where the ECMAScript regex `pattern` matches ANYWHERE in the string
 * (std::regex_search; the query DSL's `~` semantics). NULL if the pattern
 * fails to compile. */
DFTU_EXPORT dftu_series* dftu_series_str_search(const dftu_series* v,
                                                const char* pattern,
                                                int32_t pattern_len);
/** True where the string matches the SQL LIKE / glob `pattern`: `%` matches any
 * run (including empty), `_` matches exactly one character, `\` escapes a
 * literal `%`/`_`/`\`; other bytes are literal. A Bool column carrying v's
 * validity. Affix patterns (`text%`, `%text`, `%text%`, exact) take a fast
 * equality / affix / SIMD-substring route; interior wildcards use a per-row
 * two-pointer matcher. */
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
typedef enum {
    DFTU_STR_CAPITALIZE = 0,
    DFTU_STR_TITLE_CASE = 1,
    DFTU_STR_SWAPCASE = 2
} dftu_str_case;
/** `op` is a dftu_str_case. */
DFTU_EXPORT dftu_series* dftu_series_str_case(const dftu_series* v, int32_t op);

/** The character classes of dftu_series_str_is, Python's str.isalnum /
 * isalpha / isdigit / isdecimal / isnumeric / isspace / islower / isupper /
 * istitle over ASCII (DIGIT, DECIMAL and NUMERIC coincide there): a Bool
 * column, false for an empty string, null where the row is. */
typedef enum {
    DFTU_STR_ALNUM = 0,
    DFTU_STR_ALPHA = 1,
    DFTU_STR_DIGIT = 2,
    DFTU_STR_DECIMAL = 3,
    DFTU_STR_NUMERIC = 4,
    DFTU_STR_SPACE = 5,
    DFTU_STR_LOWER = 6,
    DFTU_STR_UPPER = 7,
    DFTU_STR_TITLE = 8
} dftu_str_class;
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
/** Every non-overlapping match of the regex `pattern` per row, as a
 * List<String> (an empty list where nothing matches; a null row stays null).
 * NULL for a pattern that does not compile. */
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
/** The calendar parts of dftu_series_dt_part (the pandas .dt accessor):
 * DAY_OF_WEEK is Monday = 0, DAY_OF_YEAR and ISO_WEEK are 1-based, IS_LEAP_YEAR
 * is 0 / 1, EPOCH_DAYS the day number since 1970-01-01. */
typedef enum {
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
} dftu_dt_part;
/** One calendar `part` (a dftu_dt_part) per row as Int64, of a Timestamp /
 * Date32 / Date64 / Duration column in its own unit, or of an Int64 column
 * read in `unit` (a dftu_time_unit). Naive: a timezone is ignored, every part
 * is UTC. Before the epoch the day still floors correctly. NULL for another
 * type or a bad code. */
DFTU_EXPORT dftu_series* dftu_series_dt_part(const dftu_series* v, int32_t part,
                                             int32_t unit);
/** How dftu_series_dt_round rounds to a bucket. */
typedef enum {
    DFTU_DT_FLOOR = 0,
    DFTU_DT_CEIL = 1,
    DFTU_DT_ROUND = 2
} dftu_dt_round_mode;
/** Each instant rounded to a multiple of `every` (in the column's own unit;
 * an Int64 column is taken as it is) by `mode` (a dftu_dt_round_mode; ROUND
 * is half to even, as pandas). The result keeps the input's type. NULL for a
 * date column, another non-instant type, `every` <= 0 or a bad code. */
DFTU_EXPORT dftu_series* dftu_series_dt_round(const dftu_series* v,
                                              int64_t every, int32_t mode);
/** The same instants under the timezone `tz` (a zone name such as "UTC" or
 * "Asia/Tokyo"; `tz_len` bytes, 0 for naive): the Timestamp type's zone is
 * replaced and no tick moves, as pandas `tz_convert`, or `tz_localize` on a
 * naive column with "UTC". The buffers are shared. NULL for another type. */
DFTU_EXPORT dftu_series* dftu_series_with_timezone(const dftu_series* v,
                                                   const char* tz,
                                                   int32_t tz_len);
/** Each row's List<String> joined with `sep` into one String; a null list
 * stays null. NULL unless `v` is a FLAT List of strings. */
DFTU_EXPORT dftu_series* dftu_series_list_join(const dftu_series* v,
                                               const char* sep,
                                               int32_t sep_len);

/* Reduce a FLAT numeric column to a scalar (op is a dftu_reduce_op code),
 * skipping nulls. SUM accumulates in the widest type of the column's domain
 * (i64/u64/f64) so it does not overflow narrow types or lose integer precision;
 * MIN/MAX return a value in the column's domain. */
/** Aggregation ops. A bitmask (power-of-two values) so group_by can compute
 * several in one pass; dftu_series_reduce takes a single flag (SUM/MIN/MAX). */
typedef enum {
    DFTU_REDUCE_SUM = 1,
    DFTU_REDUCE_MIN = 2,
    DFTU_REDUCE_MAX = 4,
    DFTU_REDUCE_COUNT = 8,
    DFTU_REDUCE_MEAN = 16
} dftu_reduce_op;
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

/** Materialize any encoding to a new FLAT column (gather). */
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

/** Summary statistics over the non-null values of a FLAT column. Moment stats
 * are numeric-only; quantile sorts (SIMD); nunique/unique work for any type. */
DFTU_EXPORT double dftu_series_variance(const dftu_series* v, int32_t sample);
DFTU_EXPORT double dftu_series_stddev(const dftu_series* v, int32_t sample);
DFTU_EXPORT double dftu_series_skewness(const dftu_series* v);
DFTU_EXPORT double dftu_series_kurtosis(const dftu_series* v);
DFTU_EXPORT double dftu_series_quantile(const dftu_series* v, double q);
DFTU_EXPORT int64_t dftu_series_nunique(const dftu_series* v);
/** The distinct non-null values, ascending. Caller owns the column. */
DFTU_EXPORT dftu_series* dftu_series_unique(const dftu_series* v);

/** Elementwise transforms -> a new FLAT column (caller owns). abs/clip/round
 * are SIMD; fillna clears nulls; cumsum is a running sum. */
DFTU_EXPORT dftu_series* dftu_series_abs(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_clip(const dftu_series* v, dftu_scalar lo,
                                          dftu_scalar hi);
DFTU_EXPORT dftu_series* dftu_series_round(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_fillna(const dftu_series* v,
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

/** is_unique[i] is true iff v's value at row i occurs exactly once;
 * is_duplicated is its complement (true for a repeated value). Both are
 * bit-packed Bool columns (hash pass; polars semantics). NULL rows form one
 * group. */
DFTU_EXPORT dftu_series* dftu_series_is_unique(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_is_duplicated(const dftu_series* v);

/** Whether `v` is sorted ascending (or descending when `descending` is
 * nonzero); a single scan. An empty or single-row column is sorted. */
DFTU_EXPORT int32_t dftu_series_is_sorted(const dftu_series* v,
                                          int32_t descending);

/** Drop null rows, returning a SELECTION over `v` of the non-null rows. */
DFTU_EXPORT dftu_series* dftu_series_drop_nulls(const dftu_series* v);

/** Bit-packed Bool mask, true where `v`'s value is present in the `values`
 * column (a hash-set probe). NULL rows are never in the set. Numeric `v`
 * matches numeric `values`; String `v` matches String `values`. */
DFTU_EXPORT dftu_series* dftu_series_is_in(const dftu_series* v,
                                           const dftu_series* values);

/** Sort the rows of `v` ascending (or descending), as a new FLAT column:
 * take(argsort(v, descending)). */
DFTU_EXPORT dftu_series* dftu_series_sort(const dftu_series* v,
                                          int32_t descending);

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
typedef enum {
    DFTU_RANK_AVERAGE = 0,
    DFTU_RANK_MIN = 1,
    DFTU_RANK_DENSE = 2,
    DFTU_RANK_ORDINAL = 3,
    DFTU_RANK_MAX = 4
} dftu_rank_method;

/** Rank of each row (`method` is a dftu_rank_method:
 * DFTU_RANK_AVERAGE/MIN/DENSE/ORDINAL/MAX), ascending unless `descending`.
 * Returns a Float64 column; a null value has a null rank. */
DFTU_EXPORT dftu_series* dftu_series_rank(const dftu_series* v,
                                          dftu_rank_method method,
                                          int32_t descending);

/** Reduction for dftu_series_rolling. */
typedef enum {
    DFTU_ROLLING_SUM = 0,
    DFTU_ROLLING_MEAN = 1,
    DFTU_ROLLING_MIN = 2,
    DFTU_ROLLING_MAX = 3
} dftu_rolling_op;

/** A string predicate against one literal pattern (dftu_expr_str_pred). */
typedef enum {
    DFTU_STR_PRED_CONTAINS = 0,
    DFTU_STR_PRED_STARTS_WITH = 1,
    DFTU_STR_PRED_ENDS_WITH = 2,
    DFTU_STR_PRED_LIKE = 3,
    DFTU_STR_PRED_MATCHES = 4,
    DFTU_STR_PRED_SEARCH = 5
} dftu_str_pred_op;

/** A String -> String map with no argument (dftu_expr_str_map). */
typedef enum {
    DFTU_STR_MAP_LOWER = 0,
    DFTU_STR_MAP_UPPER = 1,
    DFTU_STR_MAP_STRIP = 2,
    DFTU_STR_MAP_LSTRIP = 3,
    DFTU_STR_MAP_RSTRIP = 4
} dftu_str_map_op;

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

/** Bin each value into the half-open intervals defined by the ascending numeric
 * `breaks` column: bin index = count of breaks <= x, in 0..breaks length.
 * Returns Int32; a null input row yields a null bin. */
DFTU_EXPORT dftu_series* dftu_series_cut(const dftu_series* v,
                                         const dftu_series* breaks);
/** Like dftu_series_cut, but the edges are the `q`-quantiles of the column (q
 * buckets, bins 0..q-1). Returns Int32; a null input row yields a null bin. */
DFTU_EXPORT dftu_series* dftu_series_qcut(const dftu_series* v, int32_t q);

/** For each element of `values`, the lower-bound insertion index into `v`
 * (assumed sorted ascending) that keeps it sorted. Returns an Int64 column of
 * length values length. */
DFTU_EXPORT dftu_series* dftu_series_search_sorted(const dftu_series* v,
                                                   const dftu_series* values);

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
 * ordered string compare, a referenced field absent from the batch) - the
 * caller should fall back to the scan-time evaluator - or on error. */
DFTU_EXPORT dftu_series* dftu_dataframe_mask(const dftu_query* q,
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
DFTU_EXPORT dftu_dataframe* dftu_dataframe_with_column(const dftu_dataframe* df,
                                                       const char* name,
                                                       const dftu_series* col);

/** Drop every row that is null in ANY column. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_drop_nulls(const dftu_dataframe* df);
/** Fill nulls in every column with `value`; columns whose type rejects the
 * scalar (string/bool) are carried over unchanged. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_fill_null(const dftu_dataframe* df,
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
/** Per-column direction form: `descending[i]` (0/1) applies to `names[i]`.
 * `descending_n` must be 1 (broadcast to every key) or equal `n`. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_multi_per_col(
    const dftu_dataframe* df, const char* const* names, int32_t n,
    const int32_t* descending, int32_t descending_n);
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
typedef enum {
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
} dftu_groupwise_op;
/** `kind` (a dftu_groupwise_op) over the groups of the `n_keys` `keys`
 * columns. CUMSUM / CUMPROD (Float64) / CUMMAX / CUMMIN / DIFF / PCT_CHANGE /
 * RANK take every numeric non-key column, SHIFT every non-key column, each
 * output under the column's own name; CUMCOUNT / NGROUP are one Int64 column of
 * that name; HEAD / TAIL / NTH keep the input columns of the selected rows;
 * FFILL / BFILL fill each non-key column's nulls within the group from the
 * nearest present value before / after; ROLLING_SUM / MEAN / MIN / MAX reduce
 * the trailing `n` rows of each numeric non-key column within the group, null
 * until the window holds `n` present values. `n` is the SHIFT periods
 * (negative looks ahead), the HEAD / TAIL count, the NTH position (0-based,
 * negative from the end) or the rolling window; `rank_method` (a
 * dftu_rank_method) and `ascending` apply to RANK. A null value stays null.
 * Runs the window kernel the utilities library registers as
 * `dftu.frame.window`. NULL if `df` is NULL, `kind` is not a transform, a key
 * is absent, NGROUP has no keys or no column is eligible. */
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
                                                   const dftu_query* q);

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

/** One aggregate for dftu_dataframe_group_by_dynamic: `op` is
 * sum|min|max|count|mean, `column` the value column (ignored / may be NULL for
 * count), `out` the result column name. */
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
typedef enum {
    DFTU_CONCAT_VERTICAL = 0,
    DFTU_CONCAT_DIAGONAL = 1
} dftu_concat_how;

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
 * only); CROSS pairs every left row with every right row (no keys). */
typedef enum {
    DFTU_JOIN_INNER = 0,
    DFTU_JOIN_LEFT = 1,
    DFTU_JOIN_RIGHT = 2,
    DFTU_JOIN_OUTER = 3,
    DFTU_JOIN_SEMI = 4,
    DFTU_JOIN_ANTI = 5,
    DFTU_JOIN_CROSS = 6
} dftu_join_how;

/** Hash join `df` (left) with `other` (right) on the `n` key pairs
 * `left_on[i]` = `right_on[i]`, compared exactly (a null key never matches,
 * and each pair must share a type). Output columns: every left column, then
 * every right column except a key whose name equals its left key (that column
 * is emitted once, coalesced for OUTER); another right column whose name
 * collides with a left column gets `suffix` appended (NULL = "_right").
 * Matched rows keep left order, then a right-preserving join appends the
 * unmatched right rows. CROSS ignores the keys. NULL if a key column is
 * absent, a key pair's types differ, `n` is 0 for a keyed join, or `how` is
 * not a dftu_join_how. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_join(const dftu_dataframe* df,
                                                const dftu_dataframe* other,
                                                const char* const* left_on,
                                                const char* const* right_on,
                                                int32_t n, dftu_join_how how,
                                                const char* suffix);

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
/** A String -> String map (op is a dftu_str_map_op). */
DFTU_EXPORT dftu_expr* dftu_expr_str_map(int32_t op, const dftu_expr* a);
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
 * valid and re-runnable. `morsel_rows` is the scan chunk size; <= 0 means auto.
 * NULL on error. */
DFTU_EXPORT dftu_dataframe* dftu_lazyframe_collect(dftu_lazyframe* lf,
                                                   int64_t morsel_rows);

/** Free a lazyframe handle. */
DFTU_EXPORT void dftu_lazyframe_free(dftu_lazyframe* lf);

/** Output column names without running the query, joined by '\n' into a new
 * malloc'd C string (empty for a plan ending in a data-dependent op). The
 * caller frees it with dftu_query_string_free. NULL on error. */
DFTU_EXPORT char* dftu_lazyframe_schema(const dftu_lazyframe* lf);

/** The optimized plan as text (one op per line), for introspection. A new
 * malloc'd C string the caller frees with dftu_query_string_free. NULL on
 * error. */
DFTU_EXPORT char* dftu_lazyframe_explain(const dftu_lazyframe* lf);

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

/** First / last `n` rows (head is the row-limit form). */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_head(const dftu_lazyframe* lf,
                                                int64_t n);
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_tail(const dftu_lazyframe* lf,
                                                int64_t n);

/** Drop every row that is null in ANY column. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_drop_nulls(const dftu_lazyframe* lf);

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

/** Replace column names positionally with the `n` `names`, keeping column
 * order. `n` must match the frame's column count; a mismatch surfaces as a
 * collect error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_rename(const dftu_lazyframe* lf,
                                                  const char* const* names,
                                                  int32_t n);

/** Keep `len` rows starting at `offset` (negative `offset` counts from the
 * end, as DataFrame::slice). */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_slice(const dftu_lazyframe* lf,
                                                 int64_t offset, int64_t len);

/** Replace every null with `value`, cast to each column's type. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_fill_null(const dftu_lazyframe* lf,
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
 * unless `descending` (nulls last in both directions), as
 * DataFrame::sort_by_multi. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sort_by_multi(
    const dftu_lazyframe* lf, const char* const* by, int32_t n,
    int32_t descending);

/** Join `lf` (left) with `other` (right), as dftu_dataframe_join. The right
 * plan is collected in full when the joined plan runs (the hash build side);
 * the left plan streams through it morsel by morsel. `other` is borrowed: the
 * returned plan holds its own copy. NULL if either handle is NULL, `n` is 0
 * for a keyed join, or `how` is not a dftu_join_how; an absent key column or a
 * key type mismatch surfaces as a collect error. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_join(const dftu_lazyframe* lf,
                                                const dftu_lazyframe* other,
                                                const char* const* left_on,
                                                const char* const* right_on,
                                                int32_t n, dftu_join_how how,
                                                const char* suffix);

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
typedef enum {
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
    DFTU_WINDOW_RUNNING_PROD = 24
} dftu_window_func;

/** A FRAME_* bound that is unbounded on its side. */
#define DFTU_WINDOW_UNBOUNDED INT64_MAX

/** One appended window column: `func` over `value` (a column name, or NULL
 * when the function takes none) into `out`. `offset` is the LAG/LEAD shift,
 * the NTILE bucket count, the NTH_VALUE 1-based k or the FRAME_SUM / MIN /
 * MAX / MEAN minimum present count below which the output is null (0 =
 * none); FILL_FORWARD is the nearest present value at or before the row in
 * the partition; RUNNING_PROD the Float64 product of the present values so
 * far; `time` and `threshold`
 * feed RATE and SESSIONIZE, `counter` (nonzero) the RATE reset correction;
 * `preceding`/`following` bound a FRAME_* function in rows
 * (DFTU_WINDOW_UNBOUNDED = no bound). */
typedef struct dftu_window_spec {
    dftu_window_func func;
    const char* value;
    const char* time;
    const char* out;
    int64_t offset;
    double threshold;
    int32_t counter;
    int64_t preceding;
    int64_t following;
} dftu_window_spec;

/** How a generated gap-fill row fills its value columns; mirrors the
 * utilities GapFillMode. */
typedef enum {
    DFTU_GAP_FILL_NONE = 0,
    DFTU_GAP_FILL_LOCF = 1,
    DFTU_GAP_FILL_LINEAR = 2
} dftu_gap_fill_mode;

/** Which right row an as-of join picks; mirrors the utilities AsofDirection.
 */
typedef enum {
    DFTU_ASOF_BACKWARD = 0,
    DFTU_ASOF_FORWARD = 1,
    DFTU_ASOF_NEAREST = 2
} dftu_asof_direction;

/* ---- Provider registry --------------------------------------------------- */
/* One name-keyed registry of Source vtables, so a LazyFrame can be built by
 * name over a provider registered from anywhere in the process - a plugin
 * (dftu.svc.providers@0 forwards here), a non-plugin C caller, or a future
 * language binding. Mirrors the op registry below: process-lifetime,
 * name-keyed, and a name already taken is refused rather than replaced. */

DFTU_RESULT_DECL(dftu_result_frame, dftu_dataframe*);

/** A pull cursor over one open scan of a registered source. Exactly one
   thread drives a given cursor at a time; the host never calls next() again
   before a prior call's task (if any) has completed. */
typedef struct dftu_cursor_vt {
    /** Pull up to max_rows rows.
       Returns NULL when answered synchronously - *out is valid immediately.
       Otherwise returns a task the host awaits (see dftu_task) BEFORE reading
       *out; the task follows the same ownership as one returned from
       dftu_plugin::on_batch: freed once the host has driven it to
       completion.
       On success, DFTU_RESULT_VALUE(*out) is a NEW dftu_dataframe the host
       frees with dftu_dataframe_free, or NULL for end of stream. On failure
       *out carries a dftu_error and the host surfaces it rather than treating
       the call as end of stream. */
    dftu_task* (*next)(void* self, int64_t max_rows, dftu_result_frame* out);
    /** Release a cursor from source_vt::scan; exactly one call per scan(). */
    void (*destroy)(void* self);
    /** Optional (may be NULL). Offer `predicate` (positional against this
       cursor's own columns, borrowed for the call) as a narrowing of the
       rows it has not produced yet: a join's build side, or a node that
       learns a bound partway through, sends the key set or range it will
       keep. ADVISORY: skip only rows `predicate` is false for, or ignore it;
       every row still produced is checked again downstream, so honouring it
       changes only the work done, never the result. A node that holds an
       input cursor forwards it to in_vt->narrow when the predicate is
       positional against its input as well (a node that only adds or keeps
       columns), remapping or refusing otherwise. Same task convention as
       next(): NULL when answered inline, else a task the host awaits before
       reading *out_applied (nonzero when the cursor did anything with it,
       for diagnostics). */
    dftu_task* (*narrow)(void* self, const dftu_expr* predicate,
                         int32_t* out_applied);
    /** Optional (may be NULL). Bytes this cursor holds resident beyond one
       in-flight frame: a build table, a spool, a cache. The host sums it
       over the plan against the memory budget; without it the cursor is
       never asked to reclaim. */
    uint64_t (*bytes)(void* self);
    /** Optional (may be NULL). Free up to `want` resident bytes (spill to
       disk, compact, drop a cache) and return how many were freed; less, or
       0, is allowed. The rows the cursor produces must not change. Called
       when the plan runs past its budget, largest holder first. */
    uint64_t (*reclaim)(void* self, uint64_t want);
} dftu_cursor_vt;

/** How completely a source applied one pushed-down filter (mirrors
   dataframe::Pushed, lazyframe.h). Passed back through scan()'s out_pushed
   array, one entry per dftu_scan_request::filters. The host pre-fills every
   entry with DFTU_PUSHED_NO before calling scan() and treats a value outside
   this set as DFTU_PUSHED_NO rather than trusting it - a source has no way to
   prove an Exact claim, so a wrong one silently drops rows and must never be
   taken on faith. */
typedef enum {
    DFTU_PUSHED_NO = 0,      /**< Not applied; the host applies it. */
    DFTU_PUSHED_INEXACT = 1, /**< Pruned I/O but did not filter survivors. */
    DFTU_PUSHED_EXACT = 2,   /**< Fully applied; the host drops it. */
} dftu_pushed;

/** A pushdown request passed to dftu_source_vt::scan (mirrors
   dataframe::ScanRequest, lazyframe.h). */
typedef struct dftu_scan_request {
    /** Columns the plan needs, in the order the scan must return them. NULL
       or n_projection == 0 means all source columns. Accepting a non-empty
       projection means every returned dftu_dataframe MUST carry exactly
       these columns, in this order. */
    const char* const* projection;
    int32_t n_projection;
    /** Candidate predicates, borrowed for the call, each positional against
       the request's column set (projection when non-empty, else schema()).
       Translate what can be pushed and report the rest DFTU_PUSHED_NO. */
    const dftu_expr* const* filters;
    int32_t n_filters;
    int64_t limit;          /**< Slice pushdown hint; -1 means none. */
    uint64_t memory_budget; /**< Bytes; most sources ignore it. */
} dftu_scan_request;

/** Opaque per-column type declaration built during dftu_source_vt::
   schema_types (mirrors dataframe::Schema/DataType, types.h). The host
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
   schema a caller was only handed (dftu_node_vt::output_schema's `in`), which
   otherwise exposes no way to enumerate what an upstream stage declared. */
DFTU_EXPORT int32_t dftu_schema_field_count(const dftu_schema* schema);

/** Appends top-level field `i` of `schema` (name, type, and every parameter,
   including nested children) as a new field of `out`. Returns the new
   field's index in `out` (same convention as dftu_schema_add_field), or -1 if
   `out`/`schema` is NULL or `i` is out of range. The pass-through a node's
   output_schema uses to declare a column unchanged from its input. */
DFTU_EXPORT int32_t dftu_schema_copy_field(dftu_schema* out,
                                           const dftu_schema* schema,
                                           int32_t i);

/** Which op a dftu_apply_request offers (mirrors the Source::apply_* hooks,
   lazyframe.h). */
typedef enum {
    DFTU_APPLY_FILTER = 0,
    DFTU_APPLY_PROJECTION = 1,
    DFTU_APPLY_AGGREGATION = 2,
    DFTU_APPLY_SORT = 3,
    DFTU_APPLY_TOPN = 4,
    DFTU_APPLY_LIMIT = 5,
    DFTU_APPLY_TAIL = 6,
    DFTU_APPLY_JOIN = 7,
} dftu_apply_kind;

/** How a source took an offered op (mirrors dataframe::ApplyStatus). The host
   pre-fills DFTU_APPLY_NO_CHANGE and treats any value outside this set as
   DFTU_APPLY_NO_CHANGE. */
typedef enum {
    DFTU_APPLY_NO_CHANGE = 0, /**< Unsupported or no change; no new source. */
    DFTU_APPLY_EXACT = 1,     /**< Fully applied; the host drops the op. */
    DFTU_APPLY_INEXACT = 2,   /**< Rows narrowed only; the host keeps the op. */
} dftu_apply_status;

/** One projection output column: `expr` is positional against the offering
   source's schema(). */
typedef struct dftu_named_expr {
    const char* name;
    const dftu_expr* expr;
} dftu_named_expr;

/** One aggregate of an offered group-by (mirrors dataframe::AggregateExpr).
   `op` is the canonical aggregate name (dataframe::to_string(Agg)). `input`
   and `by` are positional against schema(); `input` is NULL for count and
   `by` is NULL unless the op reads a second column. `param` is the quantile or
   k where the op takes one. */
typedef struct dftu_apply_agg {
    const char* op;
    const dftu_expr* input;
    const dftu_expr* by;
    double param;
    const char* out;
} dftu_apply_agg;

/** FILTER: keep the rows where `predicate`, positional against schema(),
   is true. */
typedef struct dftu_apply_filter_args {
    const dftu_expr* predicate;
} dftu_apply_filter_args;

/** PROJECTION: the output is exactly `exprs[n_exprs]`, in order. */
typedef struct dftu_apply_projection_args {
    const dftu_named_expr* exprs;
    int32_t n_exprs;
} dftu_apply_projection_args;

/** AGGREGATION: group by `keys[n_keys]` and compute `aggs[n_aggs]`. The
   output is one column per key, named by its `name`, in order, then one
   column per agg named by its `out`. */
typedef struct dftu_apply_aggregation_args {
    const dftu_named_expr* keys;
    int32_t n_keys;
    const dftu_apply_agg* aggs;
    int32_t n_aggs;
} dftu_apply_aggregation_args;

/** SORT: order by `by[n_by]`, `descending[i]` nonzero for descending. */
typedef struct dftu_apply_sort_args {
    const char* const* by;
    const int32_t* descending;
    int32_t n_by;
} dftu_apply_sort_args;

/** TOPN: the first `k` rows of SORT by `sort`. */
typedef struct dftu_apply_topn_args {
    dftu_apply_sort_args sort;
    int64_t k;
} dftu_apply_topn_args;

/** LIMIT: rows [`offset`, `offset + n`) in source order. */
typedef struct dftu_apply_limit_args {
    int64_t offset;
    int64_t n;
} dftu_apply_limit_args;

/** TAIL: the last `n` rows in source order. */
typedef struct dftu_apply_tail_args {
    int64_t n;
} dftu_apply_tail_args;

/** JOIN: this source joined with `other_self`/`other_vt`, a source of the same
   provider with nothing left to run above it, on `left_on[n_on]` =
   `right_on[n_on]` by `how` (dftu_join_how), suffixing clashing right columns
   with `suffix`. The output is what the engine's join makes for the same
   arguments. `other_self` is borrowed for the call; the host keeps the other
   source alive for as long as the derived source. `other_vt` is the host's
   copy of that source's vtable, so recognize your own source by one of its
   callbacks (for example `other_vt->apply`), not by the vtable address. */
typedef struct dftu_apply_join_args {
    void* other_self;
    const struct dftu_source_vt* other_vt;
    const char* const* left_on;
    const char* const* right_on;
    int32_t n_on;
    int32_t how; /**< dftu_join_how */
    const char* suffix;
} dftu_apply_join_args;

/** An op offered to dftu_source_vt::apply: `kind` selects the one member of
   `u` that is set. Every pointer is borrowed for the call. */
typedef struct dftu_apply_request {
    int32_t kind; /**< dftu_apply_kind */
    union {
        dftu_apply_filter_args filter;
        dftu_apply_projection_args projection;
        dftu_apply_aggregation_args aggregation;
        dftu_apply_sort_args sort;
        dftu_apply_topn_args topn;
        dftu_apply_limit_args limit;
        dftu_apply_tail_args tail;
        dftu_apply_join_args join;
    } u;
} dftu_apply_request;

/** The answer to dftu_source_vt::apply. For DFTU_APPLY_EXACT or
   DFTU_APPLY_INEXACT, `self` and `vt` are a NEW derived source the host owns:
   it calls vt->destroy(self) exactly once, after every cursor it opened and
   every source derived from it are gone. `vt` must outlive that source.
   DFTU_APPLY_NO_CHANGE returns no source. */
typedef struct dftu_apply_result {
    int32_t status; /**< dftu_apply_status */
    void* self;
    const struct dftu_source_vt* vt;
} dftu_apply_result;

/** A pushdown-aware data source a plugin registers under a name. Immutable:
   schema() reports columns without scanning and scan() may be called many
   times to open independent cursors, so one registered source can back many
   LazyFrame collects. */
typedef struct dftu_source_vt {
    /** Column names, as a NUL-terminated array owned by the source and valid
       for its lifetime (until destroy()). Returns the count, or -1 on
       failure. */
    int32_t (*schema)(void* self, const char* const** out_names);
    /** Open a cursor honoring `req`. `out_pushed` has req->n_filters entries,
       pre-filled DFTU_PUSHED_NO by the host; write one dftu_pushed per
       req->filters entry, leaving an untranslated filter DFTU_PUSHED_NO.
       Returns non-NULL on success or NULL on failure; the returned pointer is
       only a status sentinel, since a stateless cursor's own `self` may
       legitimately be NULL. On success, *out_cursor_self receives the
       cursor's `self` and *out_vt its vtable, which must outlive the cursor.
       When req->projection is non-empty, every dftu_dataframe the cursor
       produces (sync or via the awaited task) MUST carry exactly those
       columns, in that order; the host verifies this and surfaces a scan
       error rather than passing a mismatched frame downstream. */
    void* (*scan)(void* self, const dftu_scan_request* req, int32_t* out_pushed,
                  void** out_cursor_self, const dftu_cursor_vt** out_vt);
    /** Release the source; called once, after every cursor it opened has
       been destroyed. */
    void (*destroy)(void* self);
    /** Optional (NULL is fine): declare column types by appending exactly
       schema()'s columns, in the same order and with the same names, to
       `out_schema` - an empty dftu_schema the host allocates and frees
       around this one call. A count or name mismatch is discarded: every
       column reports Unknown, the same as if this were NULL. */
    void (*schema_types)(void* self, dftu_schema* out_schema);
    /** Optional (NULL means no planning hooks): absorb the op `req` offers at
       plan time, before scan(). `out` is pre-filled DFTU_APPLY_NO_CHANGE.
       Accepting yields a new derived source (see dftu_apply_result); offering
       the same op to that derived source again must answer
       DFTU_APPLY_NO_CHANGE. PROJECTION, AGGREGATION and JOIN change the
       schema and may only be DFTU_APPLY_EXACT; the host refuses an INEXACT
       answer to them. The derived source's schema/schema_types report its
       output. */
    void (*apply)(void* self, const dftu_apply_request* req,
                  dftu_apply_result* out);
} dftu_source_vt;

/** Register `vt`/`self` as a named provider. `vt` is copied, so it need not
 * outlive the call, but `self` must outlive every scan opened against the
 * provider until dftu_provider_unregister removes it. Returns 0 on success,
 * non-zero if `name`/`vt` is NULL or `name` is already registered - a second
 * registration under the same name is refused, never a silent replace. */
DFTU_EXPORT int dftu_provider_register(const char* name,
                                       const dftu_source_vt* vt, void* self);

/** Remove a provider added with dftu_provider_register. A caller that can
 * unload (a plugin's shared object) must call this before unloading, the same
 * requirement dftu_op_unregister documents for a user op - otherwise a later
 * dftu_lazyframe_from_provider or in-flight scan calls into unmapped memory.
 * A no-op (returns nonzero) if `name` was never registered. */
DFTU_EXPORT int dftu_provider_unregister(const char* name);

/** Build a LazyFrame scanning the provider registered as `name`. Returns a new
 * owned dftu_lazyframe (free with dftu_lazyframe_free), or NULL if no provider
 * is registered under that name. The provider must stay registered for as
 * long as the returned LazyFrame, or any LazyFrame derived from it, may still
 * be collected. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_from_provider(const char* name);

/* ---- Op registry -------------------------------------------------------- */
/* One name-keyed registry over the engine's ops so a built-in op and a user op
 * are looked up and run the same way (the plugin-ABI foundation). Built-in ops
 * carry a plain name (`add`); a user op should namespace with a module prefix
 * (`mymod.zscore`) so it never shadows a built-in. */

/** Coarse category, for filtering/listing (== dftu_op_kind_of(sig)). */
typedef enum {
    DFTU_OP_KIND_SERIES = 0, /**< column(s) -> column */
    DFTU_OP_KIND_AGGREGATE,  /**< column -> scalar (a reducer) */
    DFTU_OP_KIND_FRAME,      /**< table(s) -> table */
    DFTU_OP_KIND_LAZY        /**< lazy plan -> lazy plan (a plan node) */
} dftu_op_kind;

/** Operand/return tokens, one 5-bit field each, so 32 values exist in total. A
 * signature packs a return token plus up to DFTU_OP_MAX_ARGS operand tokens
 * into one integer (DFTU_OP_SIG6), the way a Linux ioctl number packs
 * direction/type/nr/size. APPEND ONLY - a token's value is ABI-stable, and a
 * value past 31 would silently corrupt every signature it appears in. */
typedef enum {
    DFTU_TOK_NONE = 0, /**< empty slot */
    DFTU_TOK_SERIES,   /**< a column operand, or a column return */
    DFTU_TOK_SCALAR,   /**< dftu_scalar operand, or a dftu_scalar return */
    DFTU_TOK_I64,      /**< int64 operand, or an int64 return */
    DFTU_TOK_BOOL,     /**< int32 0/1 return */
    DFTU_TOK_CMP,      /**< dftu_cmp_op operand */
    DFTU_TOK_PRIM,     /**< dftu_prim_op operand */
    DFTU_TOK_LOGICAL,  /**< dftu_logical_op operand */
    DFTU_TOK_DTYPE,    /**< dftu_dtype operand */
    DFTU_TOK_REDUCE,   /**< dftu_reduce_op operand */
    DFTU_TOK_STR,      /**< (const char*, int32 length) operand */
    DFTU_TOK_CHAR,     /**< char operand */
    DFTU_TOK_F64,      /**< double operand, or a double return */
    DFTU_TOK_I32,      /**< int32 flag operand (e.g. a `descending` flag) */
    DFTU_TOK_RANK,     /**< dftu_rank_method operand */
    DFTU_TOK_ROLLING,  /**< dftu_rolling_op operand */
    DFTU_TOK_FRAME, /**< a dftu_dataframe operand, or a dftu_dataframe return */
    DFTU_TOK_STRLIST, /**< a (const char* const*, int32 count) string-list
                         operand */
    DFTU_TOK_I32LIST, /**< a (const int32_t*, int32 count) int32-list operand */
    DFTU_TOK_LAZY,    /**< a dftu_lazyframe operand, or a dftu_lazyframe return
                       */
    DFTU_TOK_EXPR,    /**< a const dftu_expr* operand (a predicate or a
                         projection), borrowed for the call */
    DFTU_TOK_AGGLIST, /**< a (const dftu_group_agg*, int32 count) aggregate-spec
                         list operand */
    DFTU_TOK_U64,     /**< uint64 operand (a byte count, a seed), distinct from
                         I64 so a signature describes the real parameter type
                         and cannot collide with a signed-shaped op */
    DFTU_TOK_I64LIST, /**< a (const int64_t*, int32 count) int64-list operand
                         (e.g. LazyFrame::take's row indices) */
    DFTU_TOK_QUERY,   /**< a const dftu_query* operand (a compiled predicate),
                         borrowed for the call */
    DFTU_TOK_WINLIST  /**< a (const dftu_window_spec*, int32 count) window-spec
                         list operand */
} dftu_op_tok;

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
    const char* name; /**< registry key, e.g. "add" or "mymod.zscore" */
    dftu_op_sig sig;  /**< the packed signature the runner dispatches on */
    const void* fn;   /**< the engine function pointer */
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

/** The registered op named `name` (built-in or user), or NULL if none. */
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
 * or `fn` dangles in a registry entry no one can remove it from. */
DFTU_EXPORT int dftu_op_register(const dftu_op_desc* desc);

/** Remove a user op previously added with dftu_op_register. A no-op (returns
 * non-zero) if `name` is NULL, unregistered, or a built-in - built-ins can
 * never be removed this way. Safe to call for a name that was never
 * registered. */
DFTU_EXPORT int dftu_op_unregister(const char* name);

/** One operand slot. Only the union member the operand's token names is read:
 * SCALAR->scalar, I64->i64, F64->f64, CHAR->ch, an enum token (CMP/PRIM/
 * LOGICAL/DTYPE/REDUCE/I32/RANK/ROLLING)->i32, STR->str, STRLIST->list,
 * I32LIST->i32list, I64LIST->i64list, QUERY->query, and a frame op's SERIES
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
    const dftu_query* query;
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

/* ---- Node registry (plugin plan nodes) ----------------------------------- */
/* dftu_op_run_lazy (above) runs a LAZY-kind op eagerly at the call site and
 * returns a new plan. A node is different: it is a step the ENGINE executes
 * later, inside the pull chain a collect()/stream() drives, the same protocol
 * a built-in op (filter, select, group_by, ...) already runs on - a Cursor
 * pulled with next(). One name-keyed registry, mirroring the provider
 * registry above: process-lifetime, named, and a name already taken is
 * refused rather than replaced. */

/** A cursor-shaped plan node a plugin registers under a name, so
 * dftu_lazyframe_op(lf, name, args) becomes a step lower_cursor_chain stacks
 * on the plan like any other op. A streaming node transforms each morsel and
 * passes it on; a breaker node drains its input on the first next() and emits
 * one frame - both are ordinary Cursor implementations on the plugin side, so
 * one protocol covers both shapes. */
typedef struct dftu_node_vt {
    /** Declare the node's output schema given the plan's input schema and
       `args`, without running: append to `out` with the dftu_schema builders,
       the same convention as dftu_source_vt::schema_types. Required (unlike a
       source's optional schema_types) - a node has no separate name-only
       schema() call, so this is the only way the plan learns what columns it
       produces. A column whose type the node cannot determine statically is
       appended with DFTU_TYPE_UNKNOWN rather than guessed. */
    void (*output_schema)(void* self, const dftu_schema* in,
                          const dftu_op_arg* args, dftu_schema* out);
    /** Open an output cursor over an already-open input cursor. Same
       convention as dftu_source_vt::scan: returns non-NULL on success (a
       status sentinel, since a stateless cursor's own `self` may legitimately
       be NULL) and writes *out_cursor_self and *out_vt, which must outlive
       the cursor. `in_cursor_self`/`in_vt` is the upstream stage, already
       open:
       pull it with in_vt->next(in_cursor_self, ...) the same way the engine
       pulls any cursor. On success the node takes ownership of the input
       cursor and must call in_vt->destroy(in_cursor_self) exactly once,
       whenever the node's own output cursor is destroyed (or earlier, once
       fully drained); on failure (returning the NULL sentinel) the node must
       not have taken that ownership - the caller still owns and destroys the
       input cursor. */
    void* (*open)(void* self, void* in_cursor_self, const dftu_cursor_vt* in_vt,
                  const dftu_op_arg* args, void** out_cursor_self,
                  const dftu_cursor_vt** out_vt);
    /** Release the node; called once, after every cursor opened against it
       has been destroyed. May be NULL for a stateless node. */
    void (*destroy)(void* self);
} dftu_node_vt;

/** Register `vt`/`self` as a named plan node. Same contract as
 * dftu_provider_register: `vt` is copied so it need not outlive the call, but
 * `self` must outlive every cursor opened against the node until
 * dftu_node_unregister removes it. Returns 0 on success, non-zero if
 * `name`/`vt` is NULL, `vt->output_schema` or `vt->open` is NULL, or `name` is
 * already registered - a second registration under the same name is refused,
 * never a silent replace. */
DFTU_EXPORT int dftu_node_register(const char* name, const dftu_node_vt* vt,
                                   void* self);

/** Remove a node added with dftu_node_register. A caller that can unload (a
 * plugin's shared object) must call this before unloading, the same
 * requirement dftu_provider_unregister documents for a provider - otherwise a
 * later dftu_lazyframe_op call, or a plan still holding the node from an
 * earlier call, opens or drives a cursor into unmapped memory. A no-op
 * (returns nonzero) if `name` was never registered. */
DFTU_EXPORT int dftu_node_unregister(const char* name);

/** Append a step running the node registered as `name` over `lf`, with `args`
 * (NULL when the node needs none) copied into the plan by value - a
 * pointer-bearing operand (str/strlist/i32list/i64list/agglist/expr/query)
 * must therefore point at storage that outlives every future execution of the
 * returned LazyFrame, not merely this call. Returns a new owned dftu_lazyframe
 * (free with dftu_lazyframe_free), or NULL if `lf`/`name` is NULL or no node
 * is registered under that name - an unknown name fails here, loudly, rather
 * than building a plan that fails later. A node is an optimization barrier:
 * the engine never pushes a filter or a projection through it and never
 * reorders around it, since it has no way to know whether that is safe for an
 * opaque node. An open() that returns the NULL sentinel, or a node whose
 * produced frame does not match the column count output_schema() declared,
 * surfaces as an error the first time the plan is driven (collect() or
 * stream()), naming the node. */
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
 * operand has no owned form (EXPR / QUERY / LAZY), or `n` does not match the
 * op's frame operands. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_frame_op(
    const dftu_lazyframe* lf, const char* name,
    const dftu_lazyframe* const* others, int32_t n, const dftu_op_arg* args,
    const char* const* out_names, int32_t n_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // DFTRACER_UTILS_DATAFRAME_ABI_H
