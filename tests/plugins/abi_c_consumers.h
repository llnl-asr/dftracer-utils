// SPDX-License-Identifier: MIT
#ifndef DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H
#define DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H

/* Entry points of the C consumers of the plugin and dataframe C ABIs; the
 * C++ test drives them. */
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Task service consumers; each builds jobs, starts one task and hands it back
 * through the last argument. */
dftu_status dftu_test_task_pipe(dftu_ctx*, const dftu_plugin_host*,
                                const int64_t*, int64_t*, dftu_task**);
dftu_status dftu_test_task_all(dftu_ctx*, const dftu_plugin_host*,
                               const int64_t*, int64_t*, dftu_task**);
dftu_status dftu_test_task_typecheck(dftu_ctx*, const dftu_plugin_host*,
                                     dftu_status*);
dftu_status dftu_test_task_all_error(dftu_ctx*, const dftu_plugin_host*,
                                     const int64_t*, int64_t*, int*,
                                     dftu_task**);
dftu_status dftu_test_task_any_first(dftu_ctx*, const dftu_plugin_host*,
                                     const int64_t*, int64_t*, int*,
                                     dftu_task**);
dftu_status dftu_test_task_any_deadline(dftu_ctx*, const dftu_plugin_host*,
                                        int*, dftu_task**);
dftu_status dftu_test_task_map(dftu_ctx*, const dftu_plugin_host*,
                               const int64_t*, int64_t*, uint64_t, uint32_t,
                               int64_t, int*, dftu_task**);
int dftu_test_task_map_max_seen(void);

typedef struct dftu_spawn_probe {
    int64_t ran;
    dftu_status fail_with; /* a negative status the function returns */
} dftu_spawn_probe;
dftu_status dftu_test_task_spawn(dftu_ctx*, const dftu_plugin_host*,
                                 dftu_spawn_probe*, dftu_job_where,
                                 dftu_task**);

typedef struct dftu_drive_report {
    int steps;
    int propagate;   /* the last step returns the failed task's status */
    dftu_status first_status;
    dftu_status second_status;
    int released;    /* times the host called the drive release */
    int empty_round; /* the first step returns OK with nothing to wait for */
} dftu_drive_report;
dftu_status dftu_test_task_drive(dftu_ctx*, const dftu_plugin_host*,
                                 dftu_drive_report*, dftu_task**);

int dftu_test_query_match(dftu_ctx*, const dftu_plugin_host*, const char*,
                          uint32_t, const dftu_dataframe*, int64_t);
int dftu_test_query_null_is_safe(const dftu_plugin_host*, const dftu_dataframe*,
                                 int64_t);
int dftu_test_sketch(const dftu_plugin_host*, double*, uint64_t*);
int dftu_test_sketch_merge(const dftu_plugin_host*, uint64_t*);
int dftu_test_ops_fnv1a(dftu_ctx*, const dftu_plugin_host*, const char*,
                        uint32_t, uint64_t*);
int dftu_test_ops_find(const dftu_plugin_host*);
int dftu_test_trace_roundtrip(dftu_ctx*, const dftu_plugin_host*, const char*,
                              const dftu_dataframe*, int*);
dftu_status dftu_test_io_open(dftu_ctx*, const dftu_plugin_host*, const char*,
                              int*, dftu_task**);
dftu_status dftu_test_io_open_existing(dftu_ctx*, const dftu_plugin_host*,
                                       const char*, int*, dftu_task**);
dftu_status dftu_test_io_write(dftu_ctx*, const dftu_plugin_host*, int,
                               const void*, uint64_t, int64_t*, dftu_task**);
dftu_status dftu_test_io_pread(dftu_ctx*, const dftu_plugin_host*, int, void*,
                               uint64_t, uint64_t, int64_t*, dftu_task**);
dftu_status dftu_test_io_close(dftu_ctx*, const dftu_plugin_host*, int,
                               dftu_task**);
/* What the C scenarios call back into: run a task to the end and give its
 * status and error text, record one check, and read the text the last refused
 * call left on the ctx. */
typedef struct dftu_test_harness {
    dftu_status (*run)(dftu_task*, char* message, uint64_t cap);
    void (*expect)(const char* what, int ok);
    void (*ctx_message)(dftu_ctx*, char* message, uint64_t cap);
} dftu_test_harness;
void dftu_test_io_fs_ops(dftu_ctx*, const dftu_plugin_host*,
                         const dftu_test_harness*, const char* dir);
void dftu_test_io_socket_ops(dftu_ctx*, const dftu_plugin_host*,
                             const dftu_test_harness*);
void dftu_test_io_linux_ops(dftu_ctx*, const dftu_plugin_host*,
                            const dftu_test_harness*, const char* dir);
void dftu_test_format_ops(dftu_ctx*, const dftu_plugin_host*,
                          const dftu_test_harness*, const char* id);
void dftu_test_fs_ops(dftu_ctx*, const dftu_plugin_host*,
                      const dftu_test_harness*, const char* dir);
dftu_status dftu_test_writer_create(dftu_ctx*, const dftu_plugin_host*,
                                    const char*, dftu_writer**);
dftu_status dftu_test_writer_open(dftu_ctx*, const dftu_plugin_host*,
                                  dftu_writer*, dftu_task**);
dftu_status dftu_test_writer_chunk(dftu_ctx*, const dftu_plugin_host*,
                                   dftu_writer*, const void*, uint64_t,
                                   dftu_task**);
dftu_status dftu_test_writer_close(dftu_ctx*, const dftu_plugin_host*,
                                   dftu_writer*, dftu_task**);
dftu_status dftu_test_merge_shards(dftu_ctx*, const dftu_plugin_host*,
                                   const char*, const char*, const char*,
                                   dftu_task**);
dftu_status dftu_test_lines_open(dftu_ctx*, const dftu_plugin_host*,
                                 const char*, uint64_t, uint64_t,
                                 dftu_line_reader**);
dftu_status dftu_test_lines_next(dftu_ctx*, const dftu_plugin_host*,
                                 dftu_line_reader*, uint32_t, dftu_line_batch*,
                                 dftu_task**);
void dftu_test_lines_close(const dftu_plugin_host*, dftu_line_reader*);
dftu_status dftu_test_chunks_open(dftu_ctx*, const dftu_plugin_host*,
                                  const char*, uint64_t, dftu_chunk_reader**);
dftu_status dftu_test_chunks_next(dftu_ctx*, const dftu_plugin_host*,
                                  dftu_chunk_reader*, dftu_bytes*, dftu_task**);
void dftu_test_chunks_close(const dftu_plugin_host*, dftu_chunk_reader*);
dftu_status dftu_test_gz_open(dftu_ctx*, const dftu_plugin_host*, const char*,
                              uint64_t, dftu_gz_writer**, dftu_task**);
dftu_status dftu_test_gz_append(dftu_ctx*, const dftu_plugin_host*,
                                dftu_gz_writer*, const void*, uint64_t);
dftu_status dftu_test_gz_close(dftu_ctx*, const dftu_plugin_host*,
                               dftu_gz_writer*, dftu_gz_summary*, dftu_task**);
dftu_status dftu_test_compress(dftu_ctx*, const dftu_plugin_host*, const char*,
                               const char*, uint64_t*, dftu_task**);
dftu_status dftu_test_decompress(dftu_ctx*, const dftu_plugin_host*,
                                 const char*, const char*, uint64_t*,
                                 dftu_task**);
dftu_status dftu_test_logical_path(dftu_ctx*, const dftu_plugin_host*,
                                   const char*, char*, uint64_t);
dftu_status dftu_test_mtime(dftu_ctx*, const dftu_plugin_host*, const char*,
                            int64_t*);
dftu_status dftu_test_size(dftu_ctx*, const dftu_plugin_host*, const char*,
                           uint64_t*);
dftu_status dftu_test_sampled_hash(dftu_ctx*, const dftu_plugin_host*,
                                   const char*, uint64_t*);

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_TESTS_UTILITIES_PLUGINS_ABI_C_CONSUMERS_H */
