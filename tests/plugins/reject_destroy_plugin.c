// SPDX-License-Identifier: MIT
/* Test-only plugin with a heap instance, a required config key and a plan_duql
 * taken from REJECT_DESTROY_PLAN. It appends 'd' to the file named by
 * REJECT_DESTROY_MARKER when its destroy runs, so a test can tell that the host
 * destroyed an instance whose load it then rejected.
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int unused;
} Inst;

static const dftu_config_key KEYS[] = {
    {"n", DFTU_VAL_I64, 1, "a required count"},
    {NULL, DFTU_CONFIG_ANY, 0, NULL},
};

static const dftu_config_key* config_keys(void* self) {
    (void)self;
    return KEYS;
}

static const char* plan_duql(void* self) {
    (void)self;
    return getenv("REJECT_DESTROY_PLAN");
}

static void destroy(void* self) {
    const char* path = getenv("REJECT_DESTROY_MARKER");
    FILE* f = path ? fopen(path, "ab") : NULL;
    if (f) {
        fputc('d', f);
        fclose(f);
    }
    free(self);
}

DFTU_PLUGIN_INFO("reject_destroy", "1.0")

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    dftu_plugin* p = &g_plugin;
    (void)h;
    (void)config;
    p->struct_size = sizeof(dftu_plugin);
    p->self = calloc(1, sizeof(Inst));
    if (!p->self) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    p->config_keys = config_keys;
    p->plan_duql = plan_duql;
    p->destroy = destroy;
    *out = p;
    return DFTU_OK;
}
