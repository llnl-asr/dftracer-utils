// SPDX-License-Identifier: MIT
#ifndef DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H
#define DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H

/* Entry points of the C consumers of the plugin and dataframe C ABIs; the
 * C++ test drives them. */
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

dftu_task* dftu_test_compose_pipe(const dftu_plugin_host*, const int64_t*,
                                  int64_t*, int*);
dftu_task* dftu_test_compose_all(const dftu_plugin_host*, const int64_t*,
                                 int64_t*, int*);
int dftu_test_compose_typecheck(const dftu_plugin_host*);
int dftu_test_query_match(const dftu_plugin_host*, const char*, uint32_t,
                          const dftu_dataframe*, int64_t);
int dftu_test_query_null_is_safe(const dftu_plugin_host*, const dftu_dataframe*,
                                 int64_t);
int dftu_test_sketch(const dftu_plugin_host*, double*, uint64_t*);
int dftu_test_sketch_merge(const dftu_plugin_host*, uint64_t*);
int dftu_test_ops_fnv1a(const dftu_plugin_host*, const char*, uint32_t,
                        uint64_t*);
int dftu_test_ops_find(const dftu_plugin_host*);
int dftu_test_trace_roundtrip(const dftu_plugin_host*, const char*,
                              const dftu_dataframe*, int*);
dftu_task* dftu_test_io_open(const dftu_plugin_host*, const char*, int*);
dftu_task* dftu_test_io_write(const dftu_plugin_host*, int, const void*,
                              uint64_t, int64_t*);
dftu_task* dftu_test_io_pread(const dftu_plugin_host*, int, void*, uint64_t,
                              uint64_t, int64_t*);
dftu_task* dftu_test_io_close(const dftu_plugin_host*, int, int*);
dftu_writer* dftu_test_writer_create(const dftu_plugin_host*, const char*);
dftu_task* dftu_test_writer_open(const dftu_plugin_host*, dftu_writer*);
dftu_task* dftu_test_writer_chunk(const dftu_plugin_host*, dftu_writer*,
                                  const void*, uint64_t);
dftu_task* dftu_test_writer_close(const dftu_plugin_host*, dftu_writer*);

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H */
