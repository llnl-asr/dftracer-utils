#ifndef DFTRACER_UTILS_CORE_COMMON_ABI_H
#define DFTRACER_UTILS_CORE_COMMON_ABI_H

/** @file
 * Generic C ABI value-or-error machinery shared by every ABI service group:
 * the portable error/condition types and the DFTU_RESULT_DECL family for
 * declaring a tagged result-or-error struct.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Portable, cross-domain error category mirroring
   dftracer::utils::Condition; a caller switches on this without learning any
   subsystem's private codes. */
typedef enum {
    DFTU_COND_UNKNOWN = 0,
    DFTU_COND_INTERNAL,
    DFTU_COND_INVALID_ARGUMENT,
    DFTU_COND_NOT_FOUND,
    DFTU_COND_IO,
    DFTU_COND_PARSE,
    DFTU_COND_COMPRESSION,
    DFTU_COND_TIMEOUT,
    DFTU_COND_UNSUPPORTED,
    DFTU_COND_CANCELLED
} dftu_condition;

/** A fallible call's error: (domain, code) is the precise identity (mirrors
   dftracer::utils::Error), `condition` the portable match key. `message` is
   BORROWED - valid only until the next call on the same host handle; copy it
   if it must outlive that. */
typedef struct dftu_error {
    uint64_t domain;
    int32_t code;
    int32_t condition; /**< a dftu_condition value */
    const char* message;
} dftu_error;

/** Declare a value-or-error tagged union result type `name` carrying a `T` on
   success; one declaration per T (two mentions of a bare `DFTU_RESULT(T)`
   would be distinct, incompatible struct types in C). See
   DFTU_RESULT_OK/VALUE/ERROR to use the result and DFTU_RESULT_MUST_CHECK to
   mark a function returning one. */
#define DFTU_RESULT_DECL(name, T) \
    typedef struct name {         \
        int32_t ok;               \
        union {                   \
            T value;              \
            dftu_error err;       \
        } u;                      \
    } name

#define DFTU_RESULT_OK(r) ((r).ok != 0)
#define DFTU_RESULT_VALUE(r) ((r).u.value)
#define DFTU_RESULT_ERROR(r) ((r).u.err)

#if defined(__GNUC__) || defined(__clang__)
#define DFTU_RESULT_MUST_CHECK __attribute__((warn_unused_result))
#else
#define DFTU_RESULT_MUST_CHECK
#endif

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_CORE_COMMON_ABI_H */
