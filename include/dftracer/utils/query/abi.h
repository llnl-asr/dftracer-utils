#ifndef DFTRACER_UTILS_QUERY_ABI_H
#define DFTRACER_UTILS_QUERY_ABI_H

#include <dftracer/utils/core/common/export.h>
#include <stdint.h>

// Stable C ABI for the query language: parse and serialize a query without the
// C++ types. Columnar execution (mask / plan) is a separate backend - see the
// dataframe C ABI (dftu_dataframe_*). Handles are owned; free with *_free.
#ifdef __cplusplus
extern "C" {
#endif

#ifndef DFTU_TYPEDEF_DFTU_QUERY
#define DFTU_TYPEDEF_DFTU_QUERY
typedef struct dftu_query dftu_query;
#endif

/** Comparison ops for the dftu_query_cmp_* builders, matching
 * dftracer::utils::query::CompareOp order. */
typedef enum {
    DFTU_QCMP_EQ = 0,
    DFTU_QCMP_NE = 1,
    DFTU_QCMP_GT = 2,
    DFTU_QCMP_LT = 3,
    DFTU_QCMP_GE = 4,
    DFTU_QCMP_LE = 5
} dftu_query_cmp_op;

/** Match ops for dftu_query_match, matching dftracer::utils::query::MatchOp
 * order. */
typedef enum {
    DFTU_QMATCH_LIKE = 0,
    DFTU_QMATCH_ILIKE = 1,
    DFTU_QMATCH_REGEX = 2,
    DFTU_QMATCH_IREGEX = 3,
    DFTU_QMATCH_ICONTAINS = 4
} dftu_query_match_op;

/** Parse a query DSL string into an owned handle, or NULL on a parse error. */
DFTU_EXPORT dftu_query* dftu_query_parse(const char* text);
DFTU_EXPORT void dftu_query_free(dftu_query* q);

/* Structured builders. Each returns a NEW owned handle (free with
 * dftu_query_free), or NULL on invalid input (e.g. an uncompilable regex), and
 * round-trips through dftu_query_to_string to the same canonical DSL string as
 * parsing the equivalent text. `op` is a dftu_query_cmp_op value; `match_op` is
 * a dftu_query_match_op value. */

/** field op int-value. */
DFTU_EXPORT dftu_query* dftu_query_cmp_i64(const char* field,
                                           dftu_query_cmp_op op, int64_t value);
/** field op float-value. */
DFTU_EXPORT dftu_query* dftu_query_cmp_f64(const char* field,
                                           dftu_query_cmp_op op, double value);
/** field op string-value. */
DFTU_EXPORT dftu_query* dftu_query_cmp_str(const char* field,
                                           dftu_query_cmp_op op,
                                           const char* value);

/** field in [values] over n integers. */
DFTU_EXPORT dftu_query* dftu_query_in_i64(const char* field,
                                          const int64_t* values, int32_t n);
/** field in [values] over n strings. */
DFTU_EXPORT dftu_query* dftu_query_in_str(const char* field,
                                          const char* const* values, int32_t n);
/** field not in [values] over n integers. */
DFTU_EXPORT dftu_query* dftu_query_not_in_i64(const char* field,
                                              const int64_t* values, int32_t n);
/** field not in [values] over n strings. */
DFTU_EXPORT dftu_query* dftu_query_not_in_str(const char* field,
                                              const char* const* values,
                                              int32_t n);

/** field match pattern (LIKE/ILIKE/REGEX/IREGEX/ICONTAINS). */
DFTU_EXPORT dftu_query* dftu_query_match(const char* field,
                                         dftu_query_match_op match_op,
                                         const char* pattern);

/* Combinators. Each CONSUMES its argument handles (they are freed; do not free
 * or reuse them afterward) and returns a NEW owned handle, or NULL on error (a
 * NULL argument frees the other and yields NULL). */

/** a and b. */
DFTU_EXPORT dftu_query* dftu_query_and(dftu_query* a, dftu_query* b);
/** a or b. */
DFTU_EXPORT dftu_query* dftu_query_or(dftu_query* a, dftu_query* b);
/** not a. */
DFTU_EXPORT dftu_query* dftu_query_not(dftu_query* a);

/** Serialize the query back to its DSL string as an owned C string (free with
 * dftu_query_string_free), or NULL. */
DFTU_EXPORT char* dftu_query_to_string(const dftu_query* q);
DFTU_EXPORT void dftu_query_string_free(char* s);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_QUERY_ABI_H
