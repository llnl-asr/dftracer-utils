// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGIN_ABI_MANIFEST_FIXTURE_H
#define DFTRACER_UTILS_PLUGIN_ABI_MANIFEST_FIXTURE_H

/* A small stand-in for the plugin ABI headers. ctest plugin_abi_manifest_self
   runs scripts/plugin_abi_manifest.py on this file alone, so a parser that
   drops a field, a union arm, an enumerator, a define, a typedef or a
   function pointer fails there rather than in the real manifest. */

#include <stdint.h>

typedef int32_t dftu_fixture_kind;
typedef struct dftu_fixture_opaque dftu_fixture_opaque;
enum { DFTU_FIX_OFF = 0, DFTU_FIX_ON = 7 };

#define DFTU_FIX_LIMIT 3
#define DFTU_FIX_MASK ((uint32_t)0xFFu)
#define DFTU_FIX_BANNER "dftu.fix.banner"

typedef struct dftu_fixture {
    uint32_t struct_size;
    dftu_fixture_kind kind;
    int a, b;
    union {
        int64_t i64;
        double f64;
    } value;
    union {
        uint32_t whole;
        uint8_t bytes[4];
        void (*on_arm)(void* ud, int32_t flags);
    };
    struct dftu_fixture_point {
        int32_t x, y;
    } point;
    void (*callback)(void* ud, int32_t flags);
} dftu_fixture;

typedef struct {
    const char* key;
    uint32_t key_len;
} dftu_fixture_member;

typedef void (*dftu_fixture_fn)(int32_t n);

typedef uint64_t dftu_fixture_size;

#endif  // DFTRACER_UTILS_PLUGIN_ABI_MANIFEST_FIXTURE_H
