#ifndef DFTRACER_UTILS_PLUGINS_ABI_VALUE_H
#define DFTRACER_UTILS_PLUGINS_ABI_VALUE_H

/** @file
 * Plugin config tree; host-owned and valid for the plugin's whole lifetime.
 * Include dftracer/utils/plugins/abi.h rather than this file directly.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DFTU_VAL_NULL = 0,
    DFTU_VAL_BOOL,
    DFTU_VAL_I64,
    DFTU_VAL_F64,
    DFTU_VAL_STR,
    DFTU_VAL_ARRAY,
    DFTU_VAL_OBJECT
} dftu_value_kind;

typedef struct dftu_value dftu_value;

typedef struct {
    const char* key; /**< NUL-terminated */
    uint32_t key_len;
    const dftu_value* value;
} dftu_member;

struct dftu_value {
    dftu_value_kind kind;
    uint32_t count;      /**< STR: byte length; ARRAY/OBJECT: child count */
    union {
        uint32_t b;      /**< BOOL: 0 or 1 */
        int64_t i64;     /**< I64 */
        double f64;      /**< F64 */
        const char* str; /**< STR: `count` bytes, NUL-terminated */
        const dftu_value* items;    /**< ARRAY: `count` values */
        const dftu_member* members; /**< OBJECT: `count` members */
    } as;
};

static inline const dftu_value* dftu_obj_get(const dftu_value* obj,
                                             const char* key) {
    if (!obj || obj->kind != DFTU_VAL_OBJECT) return NULL;
    for (uint32_t i = 0; i < obj->count; ++i)
        if (strcmp(obj->as.members[i].key, key) == 0)
            return obj->as.members[i].value;
    return NULL;
}
static inline int64_t dftu_as_i64(const dftu_value* v, int64_t dflt) {
    if (!v) return dflt;
    if (v->kind == DFTU_VAL_I64) return v->as.i64;
    if (v->kind == DFTU_VAL_F64) return (int64_t)v->as.f64;
    if (v->kind == DFTU_VAL_BOOL) return (int64_t)v->as.b;
    return dflt;
}
static inline double dftu_as_f64(const dftu_value* v, double dflt) {
    if (!v) return dflt;
    if (v->kind == DFTU_VAL_F64) return v->as.f64;
    if (v->kind == DFTU_VAL_I64) return (double)v->as.i64;
    return dflt;
}
static inline int dftu_as_bool(const dftu_value* v, int dflt) {
    if (!v) return dflt;
    if (v->kind == DFTU_VAL_BOOL) return (int)v->as.b;
    if (v->kind == DFTU_VAL_I64) return v->as.i64 != 0;
    return dflt;
}
static inline const char* dftu_as_str(const dftu_value* v, uint32_t* out_len) {
    if (v && v->kind == DFTU_VAL_STR) {
        if (out_len) *out_len = v->count;
        return v->as.str;
    }
    if (out_len) *out_len = 0;
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_VALUE_H */
