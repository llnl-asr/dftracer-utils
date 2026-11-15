// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_C_API_H
#define DFTRACER_UTILS_COMMON_C_API_H

/** @file
 * The error and call-context types every C ABI shares.
 *
 * A fallible C function returns a @ref dftu_status: negative is an error,
 * zero or positive is success (positive values such as DFTU_END carry a
 * non-error outcome). Results go through out-parameters.
 *
 * A library function (the caller calls the library) reports the error message
 * through a caller-owned @ref dftu_error. A callback (the host calls a plugin)
 * reports it through the host-owned @ref dftu_ctx it receives first.
 *
 * Enums at this boundary are fixed-width integer typedefs whose values are
 * written out, appended only and never reused.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t dftu_status;

/* Error codes are -(dstd::Condition + 1); c_api_enum_lockstep.cpp checks it. */
enum {
    DFTU_OK = 0,
    DFTU_END = 1,     /**< a cursor or iterator has no more items */
    DFTU_PENDING = 2, /**< not ready yet; the host calls again later */

    DFTU_E_UNKNOWN = -1,
    DFTU_E_INTERNAL = -2,
    DFTU_E_INVALID = -3,
    DFTU_E_NOT_FOUND = -4,
    DFTU_E_IO = -5,
    DFTU_E_PARSE = -6,
    DFTU_E_COMPRESSION = -7,
    DFTU_E_TIMEOUT = -8,
    DFTU_E_UNSUPPORTED = -9,
    DFTU_E_CANCELLED = -10,
    DFTU_E_FORKED_HANDLE = -11,
    DFTU_E_NO_MEMORY = -12 /**< an allocation or reservation was refused */
};

/** A library call's error.
 *
 * Caller-owned. Initialize with DFTU_ERROR_INIT. A failing call releases what
 * the error held, then fills it; a successful call leaves it as it was. The
 * caller calls @ref dftu_error_release once it is done with the message.
 * (domain, code) is the precise identity; `status` is the portable code.
 */
typedef struct dftu_error {
    uint64_t domain;
    int32_t code;
    dftu_status status;
    const char* message; /**< NUL-terminated; NULL when none */
    void (*release)(struct dftu_error* e); /**< frees message; NULL = static */
} dftu_error;

#define DFTU_ERROR_INIT {0, 0, DFTU_OK, NULL, NULL}

static inline void dftu_error_release(dftu_error* e) {
    if (e && e->release) e->release(e);
}

/** The host-owned context of one callback call.
 *
 * Valid only for the call. The plugin reports an error with
 * @ref dftu_fail and polls @ref dftu_check in long loops. A plugin never
 * copies or frees it.
 */
typedef struct dftu_ctx dftu_ctx;

typedef struct dftu_ctx_vt {
    uint32_t struct_size;
    /** The host copies `msg` (`len` bytes, no NUL needed) and returns `code`.
     */
    dftu_status (*set_error)(dftu_ctx* c, dftu_status code, const char* msg,
                             uint32_t len);
    /** DFTU_OK, or DFTU_E_CANCELLED when the call was cancelled. Cheap;
       call it once per morsel in a long loop and return what it gives. */
    dftu_status (*check)(dftu_ctx* c);
} dftu_ctx_vt;

struct dftu_ctx {
    uint32_t struct_size;
    uint32_t reserved;
    const dftu_ctx_vt* vt;
    void* h; /**< host state behind vt */
};

static inline dftu_status dftu_fail(dftu_ctx* c, dftu_status code,
                                    const char* msg) {
    uint32_t n = 0;
    if (msg)
        while (msg[n]) ++n;
    return c->vt->set_error(c, code, msg, n);
}

static inline dftu_status dftu_check(dftu_ctx* c) { return c->vt->check(c); }

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_COMMON_C_API_H */
