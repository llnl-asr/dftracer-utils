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
| `scale_corona_fixed.tsv` | every tool on `minife/tuolumne` at 96 and 16 threads after the fixes | `69779a9c` |
| `scale_corona_traced_fixed.tsv` | every tool on `minife/tuolumne` at 1 thread under the DFTracer preload after the fixes | `69779a9c` |
| `scale_corona_fixed2.tsv` | every tool on `minife/tuolumne` at 96 and 16 threads after the cold-view and directory-scan work | `3dd2713` |
| `scale_corona_traced_fixed2.tsv` | every tool on `minife/tuolumne` at 1 thread under the DFTracer preload after that work | `50c67be` |
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

## After the fixes (`69779a9c`, 96 threads)

| Tool | Before | After | Instructions per MB before / after | RSS before / after |
|---|---|---|---|---|
| `dftracer_index` | 544 s | 210 s | 2.94 / 1.95 G | 16.4 / 8.2 GB |
| `dftracer_index`, 16 threads | 413 s | 73 s | 2.90 / 1.93 G | 4.3 / 1.9 GB |
| `dftracer_event_count` | 101 s | 25 s | 4.33 / 1.74 G | 17.3 / 8.5 GB |
| `dftracer_info` | 91 s | 24 s | 4.39 / 1.72 G | 19.4 / 8.4 GB |
| `dftracer_stats` | crash | 49 s | - / 3.13 G | - / 8.6 GB |
| `dftracer_view` | 732 s | 208 s | 15.0 / 2.77 G | 4.9 / 4.7 GB |
| `dftracer_split` | 239 s | 24 s | 6.25 / 0.96 G | 17.2 / 3.4 GB |
| `dftracer_comparator` | crash | 80 s | - / 6.15 G | - / 28.1 GB |
| `dftracer_replay` | 259 s | 291 s | 1.40 / 1.36 G | 0.5 / 0.2 GB |

The 96-thread index row is the first run of its job, so it includes the cold Lustre metadata cache (see above).

Still open: `dftracer_replay` is serial; `dftracer_view` keeps only 3 cores busy (its index build and compaction); and `dftracer_comparator` holds 28.1 GB RSS with a 3.8 GB heap, from an aggregation that keeps about one entry per event.

## After the fixes at 1 thread under DFTracer (`69779a9c`)

| Tool | Before | After | Time in traced I/O calls after |
|---|---|---|---|
| `dftracer_index` | 1,758 s | 478 s | 185 s |
| `dftracer_event_count` | 684 s | 297 s | 33 s |
| `dftracer_info` | 675 s | 285 s | 24 s |
| `dftracer_stats` | 928 s | 512 s | 48 s |
| `dftracer_view` | 1,369 s | 457 s | 18 s |
| `dftracer_split` | 1,337 s | 177 s | 27 s |
| `dftracer_validate` | 50 s | 47 s | 7 s |
| `dftracer_pgzip` | 2.8 s | 3.6 s | 1.1 s |
| `dftracer_comparator` | 1,734 s | 1,000 s | 61 s |
| `dftracer_replay` | 203 s | 207 s | 11 s |

In `dftracer_index`, 120 s of the 185 s are reads of the input (1.26 GB in 9,203 reads); index files cost about 9 s, against 472 s for unlink alone before.
Each input is still opened about 4 times and stat'd 3.5 to 5.5 times: `dftracer_index` makes 25,209 open, 8,401 fopen64 and 29,829 stat calls on 8,404 traces (about 42 s), and `dftracer_event_count` makes 46,003 stat calls (22.5 s).
`dftracer_stats` and `dftracer_comparator` also make 636,000 and 719,000 stat calls that DFTracer records with no file name (16 s and 19 s), about 80 per trace.

## After the cold-view and directory-scan work (`3dd2713`, `50c67be`)

The 96 and 16 thread rows are from `3dd2713`. The traced rows are from `50c67be`, the first commit after it on which the tools run under the DFTracer preload: at `3dd2713` every tool overflowed its stack in a loop through the preload's `munmap` wrapper.

| Tool | 96 threads, `69779a9c` / now | 16 threads, `69779a9c` / now | 1 thread traced, `69779a9c` / now | RSS at 96 threads, `69779a9c` / now |
|---|---|---|---|---|
| `dftracer_index` | 210 / 32 s | 73 / 32 s | 478 / 168 s | 8.2 / 3.1 GB |
| `dftracer_event_count` | 25 / 13 s | 38 / 21 s | 297 / 173 s | 8.5 / 3.2 GB |
| `dftracer_info` | 24 / 8.2 s | 32 / 17 s | 285 / 167 s | 8.4 / 3.2 GB |
| `dftracer_stats` | 49 / 16 s | 67 / 28 s | 512 / 274 s | 8.6 / 6.0 GB |
| `dftracer_view` | 208 / 8.6 s | 210 / 20 s | 457 / 187 s | 4.7 / 3.2 GB |
| `dftracer_split` | 24 / 18 s | 25 / 19 s | 177 / 154 s | 3.4 / 1.9 GB |
| `dftracer_validate` | 7.7 / 1.7 s | 9.1 / 3.6 s | 47 / 53 s | 1.0 / 1.1 GB |
| `dftracer_pgzip` | 0.5 / 0.6 s | 0.4 / 0.7 s | 3.6 / 3.6 s | 0.6 / 0.6 GB |
| `dftracer_comparator` | 80 / 60 s | 111 / 74 s | 1,000 / 646 s | 28.1 / 10.4 GB |
| `dftracer_replay` | 291 / 8.7 s | 276 / 26 s | 208 / 218 s | 0.2 / 1.3 GB |

`dftracer_index` needs 1.0 G instructions per MB where it needed 1.95 G, and `dftracer_view` 1.08 G where it needed 2.77 G.
Every run of both tables passed, so their `.known.tsv` files list no failure.

