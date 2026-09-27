#ifndef DFTRACER_UTILS_DUQL_ABI_H
#define DFTRACER_UTILS_DUQL_ABI_H

#include <dftracer/utils/core/common/export.h>
#include <stdint.h>

// Stable C ABI for the query language: parse and serialize a query without the
// C++ types. Columnar execution (mask / plan) is a separate backend - see the
// dataframe C ABI (dftu_dataframe_*). Handles are owned; free with *_free.
#ifdef __cplusplus
extern "C" {
#endif

#ifndef DFTU_TYPEDEF_DFTU_DUQL
#define DFTU_TYPEDEF_DFTU_DUQL
typedef struct dftu_duql dftu_duql;
#endif

/** Comparison ops for the dftu_duql_cmp_* builders, matching
 * dftracer::utils::duql::CompareOp order. */
typedef enum {
    DFTU_DUQL_CMP_EQ = 0,
    DFTU_DUQL_CMP_NE = 1,
    DFTU_DUQL_CMP_GT = 2,
    DFTU_DUQL_CMP_LT = 3,
    DFTU_DUQL_CMP_GE = 4,
    DFTU_DUQL_CMP_LE = 5
} dftu_duql_cmp_op;

/** Match ops for dftu_duql_match, matching dftracer::utils::duql::MatchOp
 * order. */
typedef enum {
    DFTU_DUQL_MATCH_LIKE = 0,
    DFTU_DUQL_MATCH_ILIKE = 1,
    DFTU_DUQL_MATCH_REGEX = 2,
    DFTU_DUQL_MATCH_IREGEX = 3,
    DFTU_DUQL_MATCH_ICONTAINS = 4
} dftu_duql_match_op;

/** Parse a query DSL string into an owned handle, or NULL on a parse error. */
DFTU_EXPORT dftu_duql* dftu_duql_parse(const char* text);
DFTU_EXPORT void dftu_duql_free(dftu_duql* q);

/* Structured builders. Each returns a NEW owned handle (free with
 * dftu_duql_free), or NULL on invalid input (e.g. an uncompilable regex), and
 * round-trips through dftu_duql_to_string to the same canonical DSL string as
 * parsing the equivalent text. `op` is a dftu_duql_cmp_op value; `match_op` is
 * a dftu_duql_match_op value. */

/** field op int-value. */
DFTU_EXPORT dftu_duql* dftu_duql_cmp_i64(const char* field, dftu_duql_cmp_op op,
                                         int64_t value);
/** field op float-value. */
DFTU_EXPORT dftu_duql* dftu_duql_cmp_f64(const char* field, dftu_duql_cmp_op op,
                                         double value);
/** field op string-value. */
DFTU_EXPORT dftu_duql* dftu_duql_cmp_str(const char* field, dftu_duql_cmp_op op,
                                         const char* value);

/** field in [values] over n integers. */
DFTU_EXPORT dftu_duql* dftu_duql_in_i64(const char* field,
                                        const int64_t* values, int32_t n);
/** field in [values] over n strings. */
DFTU_EXPORT dftu_duql* dftu_duql_in_str(const char* field,
                                        const char* const* values, int32_t n);
/** field not in [values] over n integers. */
DFTU_EXPORT dftu_duql* dftu_duql_not_in_i64(const char* field,
                                            const int64_t* values, int32_t n);
/** field not in [values] over n strings. */
DFTU_EXPORT dftu_duql* dftu_duql_not_in_str(const char* field,
                                            const char* const* values,
                                            int32_t n);

/** field match pattern (LIKE/ILIKE/REGEX/IREGEX/ICONTAINS). */
DFTU_EXPORT dftu_duql* dftu_duql_match(const char* field,
                                       dftu_duql_match_op match_op,
                                       const char* pattern);

/* Combinators. Each CONSUMES its argument handles (they are freed; do not free
 * or reuse them afterward) and returns a NEW owned handle, or NULL on error (a
 * NULL argument frees the other and yields NULL). */

/** a and b. */
DFTU_EXPORT dftu_duql* dftu_duql_and(dftu_duql* a, dftu_duql* b);
/** a or b. */
DFTU_EXPORT dftu_duql* dftu_duql_or(dftu_duql* a, dftu_duql* b);
/** not a. */
DFTU_EXPORT dftu_duql* dftu_duql_not(dftu_duql* a);

/** Serialize the query back to its DSL string as an owned C string (free with
 * dftu_duql_string_free), or NULL. */
DFTU_EXPORT char* dftu_duql_to_string(const dftu_duql* q);
DFTU_EXPORT void dftu_duql_string_free(char* s);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_DUQL_ABI_H
