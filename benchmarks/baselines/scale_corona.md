# CLI tools on Corona: baseline and bottlenecks

Measured on Corona, one pbatch node (48 cores, 96 hardware threads), the `release` preset with gcc 12.1, by `scripts/bench/scale_ladder.py`.
Each row is one run of one tool on one trace set, read through a fresh folder of symlinks, so no tool writes into the input folders.
The test-suite metrics are from the `tests` preset (a debug build) of the same tree.

## Tables

| File | What | Code |
|---|---|---|
| `scale_corona_head.tsv` | every tool at the default thread count (96) on `minife/tuolumne` (1.09 GB, 14,117 files, 8,404 traces), all of `minife` (5.4 GB, 39,911 files), the amg subset (73 GB, 2,540 files) and the one-file genesis sweep (17 GB) | `b33cabbc+wip` |
| `scale_corona_sweep.tsv` | `dftracer_index` on `minife` at 1 to 48 threads, and every tool on `minife/tuolumne` at 96 and 16 threads | `b33cabbc+wip` |
| `scale_corona_traced.tsv` | every tool on `minife/tuolumne` at 1 thread under the DFTracer preload | `b33cabbc+wip` |
| `scale_corona_fixed.tsv` | every tool on `minife/tuolumne` at 96 and 16 threads after the fixes | `f20a7e6a` |
| `test_suite_metrics.tsv` | each ctest: wall time, parallelism, RSS, instructions, IPC, cache misses, Cachegrind D1 and LL misses, DHAT bytes and blocks | `b33cabbc+wip` |

`b33cabbc+wip` is `b33cabbc` with the memory report, the I/O backend switch and this ladder, which are now commits `4a5302d0` to `dabf2cf1`.
Each table has a `.known.tsv` beside it that names the failures measured on that code and their cause, and `scale_ladder.py --check TABLE --known TABLE.known.tsv` passes on each.

The traced table runs at 1 thread because DFTracer 2.2.0 crashes in its stdio wrappers when many threads open files at once (6 of 6 runs pass at 1 thread, 1 of 6 crashes at 4 threads, most crash at 96).

In the ladder, the first run of a job reads its inputs with a cold Lustre metadata cache.
The sweep job ran `dftracer_index` at 96 threads first, so that row (544 s) includes the cold cache; a later profile on the fixed code took 61 s at 96 threads and 72 s at 16.

## Ranked bottlenecks of `b33cabbc+wip`

Ranked by the time they cost on `minife/tuolumne`, the 1 GB set of 77 KB traces.

1. **Index file storm.** Every trace wrote its bloom tier as three sorted runs of up to four SSTs, even a 77 KB trace that never spilled: about 25 files created, synced, hard-linked and unlinked per trace.
   At 1 thread, 96% of `dftracer_index` wall time was in those metadata calls on Lustre (unlink 472 s, open 385 s, link 166 s, fdatasync 41 s of 1,758 s).
   `dftracer_view` and `dftracer_split` build the same index first.
2. **Compaction of tiny SSTs.** `dftracer_view` spent 65% of its cycles in RocksDB compaction and 45% in libzstd (level 9 with a 256 KB dictionary on every level), and read 221 times its input.
   It needed 15.1 G instructions per MB on the small files and 3.5 on the 29 MB amg files.
3. **One lock per index root.** `dftracer_stats` and `dftracer_comparator` checked every trace's index under one lock per root.
   With thousands of waiters the lock's inline hand-off overflowed the stack, so both crashed on the 1 GB and 5 GB sets.
4. **A fixed 1 MB per string table.** Each per-file string table zeroed about 1 MB of directories at birth, so `dftracer_event_count`, `dftracer_info` and `dftracer_stats` held a 10 to 12 GB heap for 1 GB of input, and page faults were about 45% of their context switches.
5. **A block cache per options call.** Each RocksDB options call built and freed a 32 MB cache: one page fault per call, two calls per SST, about 20% of `dftracer_event_count` context switches.
6. **Split memory.** `dftracer_split` let each chunk writer assume a third of the memory; its tracked mapped bytes reached 705 GiB on amg.
7. **Serial replay.** `dftracer_replay` keeps 1.6 to 1.9 cores busy (1,601 s on genesis).
8. **Flat directory scans.** `index`, `event_count`, `info`, `stats`, `split`, `comparator` and `replay` found no trace in a nested set, and `view` wrote `split/` copies beside its inputs.

From the test suite (`test_suite_metrics.tsv`): `dataframe/test_groupby_scale` is the largest by RSS (1,987 MiB) and blocks (30 M) at IPC 0.49; `dstd/test_string_intern`, `binaries/test_dftracer_view` and the `trace/views/test_view_duql*` tests run at IPC 0.38 to 0.48 over 10 G instructions or more; `trace/views/test_dynamic_prune` and `test_restart_scan` allocate about 70 GB in 1 to 4 s.

## After the fixes (`f20a7e6a`, 96 threads)

| Tool | Before | After | Heap before / after |
|---|---|---|---|
| `dftracer_index` | 544 s | 204 s | 10.4 / 1.8 GB |
| `dftracer_event_count` | 101 s | 30 s | 10.1 / 1.9 GB |
| `dftracer_info` | 91 s | 27 s | 12.2 / 1.7 GB |
| `dftracer_stats` | crash | 56 s | - / 2.4 GB |
| `dftracer_view` | 732 s | 208 s | 0.7 / 0.5 GB |
| `dftracer_split` | 239 s | 24 s | 10.2 / 0.4 GB |
| `dftracer_comparator` | crash | 87 s | - / 2.7 GB |
| `dftracer_replay` | 259 s | 279 s | 0.4 / 0.1 GB |

Still open: replay is serial; about 9% of `dftracer_index` cycles are in glibc malloc and free, outside our allocator; about 17% are in string interning; and `dftracer_comparator` holds 28.5 GB RSS with a 2.7 GB heap.
