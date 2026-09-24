# Changelog

All notable changes to dftracer-utils are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to adhere to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

<!-- changelog-body -->

## [Unreleased]

### Added

- A columnar `DataFrame` / `Series` / `LazyFrame` engine that is a drop-in for
  pandas and polars: `import dftracer.utils.pandas as pd` (or `.polars as pl`)
  and most code runs unchanged on the SIMD kernels. The pandas surface covers
  `loc` / `iloc` / `at` / `iat` and `set_index` (the index is a named column,
  copy-on-write assignment), `groupby` objects with the `agg` forms, column
  selection (`groupby(k)["v"]`), a Series key, group-wise transforms
  (`cumsum`, `shift`, `rank`, `head`, `nth`, `ffill`, `bfill`, `rolling`,
  `expanding`, `ewm`, `take`, `sample`, `resample`), `apply` / `map` compiled
  into the engine (Python per row only as a last resort, with a warning), the
  `str` and `dt` accessors, `tz_localize` / `tz_convert`, `merge`, `nlargest`,
  `value_counts`, `mode`, `compare`, `pivot_table` and `describe`; the polars
  spellings sit alongside (`select(Expr)`, `with_columns`, `over`, the `str`
  namespace). `Series` masks combine with `&`, `|` and `~`; `==` / `!=` return
  a mask (a Series is unhashable, as in pandas and polars).
- A hash join on `DataFrame`, `LazyFrame`, the C ABI and Python (`join` /
  `merge`; inner, left, right, outer, semi, anti and cross). A plan's join sends its
  build keys to the scan, which prunes the chunks that cannot match.
- Plans (`LazyFrame`): `explain()`, `schema()` and `output_schema()` without
  running; `memory_budget` / `auto_spill` bounding every breaker; a source of
  your own (`Source` / `dftu_source_vt`, registered by name); a plugin's own
  plan step (`dftu_node_register`, `LazyFrame.op`); a `frame_op` step for
  every registry table op (`unnest`, `partition_id`, `compare_agg`, `window`,
  `gap_fill`, `asof`, `interval`, `concat`, `union`, `pivot` and `to_dummies`).
- Aggregates: `prod`, `cumprod`, exact group `median` / `quantile`,
  `unique(subset)`, a per-column `reduce`, a whole-frame `group_by()`,
  `first` / `last` exact across a parallel merge; group keys of any type
  (Binary, Float16, null keys as their own group with `dropna=False`).
- Plugin ABI: a plugin transforms the batch every later plugin receives
  (`transform`), reports and releases what it holds against the memory
  budget (`bytes` / `reclaim`), and a plugin node or slice under a plan is
  measured by the same budget; `abi_version` is a hash of the header, so a
  plugin built against another version is refused at load.
- `benchmarks/dataframe_vs_pandas_polars.py`: the engine against pandas,
  polars and DuckDB on the same Arrow tables, eagerly and as a plan, with a
  correctness check of every result against ours, the cores each engine kept
  busy and `--memory` for the peak resident set per op.
- The trace `View` (C++) and `TraceViewer` (Python) are a `LazyFrame` over a
  trace scan: generic ops (filter, select, sort, head, join, ...) chain on
  it, and the scan absorbs what it can at plan time (filters, projections,
  group keys, a trailing row window). Trace terminals (`call_tree`,
  `flamegraph`, `containment`, `flamegraph_partial`, `aggregate_partial`,
  `sink_json`, `sink_trace`, `materialize`) are lazy results, and
  `collect_all` runs several of them over one shared scan. Python adds lazy
  `[]`, `LazyScalar` reductions and `LazyResult`.
- Python `Indexer(bloom=BloomConfig(fields=...))` names extra args fields to
  index.
- The index covers every flat args field by default: numbers by per-chunk
  min/max, strings by a per-chunk bloom up to 256 distinct values, so a
  filter on any arg prunes chunks (equality prunes on min/max as well).
  `dftracer_index --no-auto-dimensions` opts out. On a 2M-event trace the
  index is 1.2 MB, against 8.7 MB for the previous named defaults. An
  existing index rebuilds once.

### Changed

- The dataframe engine is measured (10M rows, Apple M4 Pro): ahead of pandas
  on every benchmark row, of polars on every row but two at the noise floor,
  of DuckDB on every row but one within a millisecond of it. The group-by
  runs a plain loop over batches of rows with a direct table for dense
  integer keys, a word table for string keys and, with many groups on a
  string key, a scatter into per-thread partitions; the join uses a
  direct-address table and 32-bit index lists; the sort is a sample sort;
  filters, comparisons, string predicates, casts, gathers, rolling windows,
  `//` / `%` / `**` and the dictionary encoder run in parallel; a quantile
  reads its column in place and sorts one bucket.
- A plan over a resident frame runs whole-column for every op (an op with no
  eager form runs its own cursor over the frame as one morsel); it no longer
  streams through a spool or spills to disk.
- Memory detection reads the free and inactive pages on macOS (the auto
  budget assumed 1 GB there).
- `Series.rolling(...).mean()` and the other windows write their output in
  place and run a chunk per thread.
- **Breaking (C++):** `View` terminals follow the `LazyFrame` names:
  `export_json` / `export_trace` / `export_counters` became `sink_json` /
  `sink_trace` / `sink_counters`, `merge_partials_to_table` became
  `merge_partials`, `limit` / `offset` became `head` / `slice`, `occ_cell`
  became `resolution` and `schema()` became `column_info()`. `collect()`
  returns the `DataFrame`; `lazy()` returns the plan. `call_tree`,
  `flamegraph`, `containment` and the partials return plans to collect.
  Several outputs over one scan use `collect_all` or `TraceSession`;
  caller folds attach with `View::branch`.
- **Breaking (Python):** `TraceViewer` is a `LazyFrame`; `AggregatedTraceViewer`
  and `SessionView` are gone. `DaskTraceViewer.occ_cell` became `resolution`,
  and its `offset` / `limit` trim the merged result instead of each shard's.
- The server, `dftracer_view`, `dftracer_run`, the statistics tools and the
  C ABI run on the same `View`; the separate builder API is internal.

### Removed

- **Breaking:** the previous plugin ABI. A plugin built against it does not
  load; rebuild against `dftracer/utils/plugins/abi/plugin.h`.
- **Breaking (C++):** `AggregatedView`, `ViewSession`'s public constructor,
  `View::join` and `trace/views/result_batch.h` (`collect_batch`); use
  `View` plans, `View::branch` and `LazyFrame::join`.

### Fixed

- A sketch quantile (`pct` in a plan, `DDSketch`) returned `-inf` once a
  bucket held more than 65535 values; buckets are 32-bit now.
- Column-column arithmetic dropped nulls; a Bool column was gathered by
  byte instead of by bit; `group_by` returned groups out of first-seen order
  after a parallel run; a plugin node was answered asynchronously when the
  data was resident.
- `df.groupby(series)` raised a `SystemError`: the frame's `in` test left a
  pending error for a non-string key.
- A scan of an unindexed multi-member trace read the lines at each member
  boundary twice.
- Index extra args fields were dropped by the fold-based build, so
  `dftracer_index --dimensions` did nothing; nested fields were dropped in the
  first-touch build; merged chunk stats ordered numeric min/max as text; and
  a float literal (`pid == 1.0`) probed the bloom as `"1.000000"` and pruned
  matching chunks.
- Session containment branches ignored `phase()`, so `phase(Events)` kept
  aggregated records; session aggregations with string-arg predicates or
  transformed keys wrote rollups that later reads served wrongly.
- A blocking `get()` on a finished coroutine task could miss its result;
  `LazyFrame::collect()` on a temporary plan read the freed plan.
- A plan's export sink flushes when the export ends.
- Data races: libdeflate chose its kernels on first use from several threads
  at once, and the reader's member decode cache read an entry's ready flag
  outside the lock that wrote it.
- An expression the column type cannot take (a string column compared with a
  number) produced a null column that crashed a later `group_by`; it now
  raises an error.

- Interactive web trace viewer gains a counter timeline track (with malformed-value
  handling), per-counter pid/tid breakdown, bounded-density serving, and active-time
  statistics.
- Rectangle selection in the viewer scopes analysis to a time range, lanes, and rows;
  aggregated (`ph=3`) events are visualized with uniform extrapolation and labeled
  "aggregated" in tooltips.
- DLIO event category and name can be remapped through an event-map file.
- Python: `DaskTraceViewer` is exported from the `dask` module; new `TraceViewer`/`View`
  bindings expose the query DSL and portable C-API glue.
- Query DSL: subsumption-based simplification, string-match operators, and
  `resolved.*` virtual fields.
- Materialized views with rollups and tier-served aggregation; a new `AggregationFold`
  and extended aggregation operators.
- Concurrency-aware occupancy aggregates `busy`, `concurrency`, `utilization` and
  `active` measure wall-clock busy time and parallelism instead of double-counting
  overlapping durations. They are field-less (always over `dur`), work per group and
  time bucket, and merge across files and ranks; exposed on the `View`/`TraceViewer`
  `agg`, the `dftracer_view --agg` CLI, and the typed `AggOp` enum.
- Multi-member split, merge, and reorganize for intra-file parallelism.
- Humanized sizes and durations: every `<bytes>` CLI flag accepts a unit suffix
  (`64MB`, `1.5GiB`, `512KB`, `8kb`; 1024-based, `b` is bits) and every `<s>` flag
  accepts a duration suffix (`30s`, `5m`, `1.5h`); a bare number keeps the flag's
  legacy unit. The Python API's byte/duration arguments accept a number or a string.
- `dftracer_server --timeout` bounds server uptime and then shuts down gracefully
  (accepts a humanized duration; `0`, the default, disables it).
- Full type coverage across the public Python surface (`Series`/`DataFrame`/viewers,
  the jit DSL, and a generic `TaskHandle`).

### Changed

- `Runtime.submit()` no longer accepts a `name=` argument; the task name is derived
  automatically from the callable and its call-site source location.

- Indexer scan core reworked to a fold-fusion, batch-native design; server query,
  viz caching, and frontend paths updated to match.
- `--select` is now applied to raw event queries (SQL-style column projection).
- Substantial internal refactors: async runtime/pipeline/cache, trace reader and
  compression I/O, RocksDB manager lifecycle, shared primitives and umbrella headers,
  domain composites (visitor pattern retired), and CLI binaries/trace generators.

### Removed

- **Breaking:** the legacy per-line visitor scan path is retired in favor of the
  batch-native fold-fusion indexer.

### Fixed

- Indexer recovers a chunk whose batch parse collapses on a single bad line, closes
  cached index handles before removing a stale index, and adds a sharded immutable
  index with a full-path-keyed registry (breaking on-disk change).
- Comparator now defaults to comparing all events rather than only POSIX/STDIO.
- Server: zero-fill simdjson padding in the viz summary fold.
- CI reworked onto a shared Flux allocation with GitLab pipelines mirroring the GitHub
  workflows; wheel version now sees tags via full-history fetch.

## [0.0.12] - 2026-07-13

### Added

- Interactive web trace viewer: a SolidJS canvas frontend backed by a new server
  visualization API, with timeline node grouping (#93), app-span events, and a
  timelapse axis for multi-run traces.
- `dftracer_stats` now shows histogram bounds.
- Indexer detects and rebuilds stale indexes when the source trace changes; this
  rebuild is honored across read consumers and the server.

### Changed

- Leaner wheels; CI compiles RocksDB once and persists ccache for faster builds.

### Fixed

- I/O: scatter-gather writes now complete past the `IOV_MAX` limit.
- Comparator no longer miscounts files.
- Fixed a deadlock in multi-process dfanalyzer and a stale-index / app-span-complement
  run break in the server.
- `dfanalyzer` distributed indexing skips already-indexed files.
- `lcov` coverage compatibility fix.

## [0.0.11] - 2026-07-05

### Added

- New `dftracer_validate` CLI tool.
- `Result<T>` error model with `DFT_TRY`, and Python bindings that map `ErrorCode`
  to typed exceptions.
- Comparator gains a DLIO preset.
- ARM64 (aarch64) build support in CI and Valgrind memory checking for the C++ and
  Python test suites.

### Changed

- **Breaking:** core runtime and I/O consolidated around the new `Result<T>` model;
  single-op utilities migrated to `Result<T>`, with god-file splits and broad dedup.
- Python bindings deduplicated; GIL ordering fixed and an `atexit` shutdown added.

### Fixed

- Executor shutdown lost-wakeup deadlock.
- Object-pool Treiber-stack races (memory ordering plus a DWCAS optimization to fix
  a segfault).
- Noisy aarch64 warnings silenced.

## [0.0.10] - 2026-06-08

### Added

- Distributed HLM filtering and time bucketing.

## [0.0.9] - 2026-06-07

### Fixed

- Wheel build: updated Xcode version for macOS packaging.

## [0.0.8] - 2026-06-05

### Added

- Portable-wheel support via the `DFTRACER_UTILS_LOCAL_PACKAGES` option.

### Fixed

- Portable `to_chars_double` fallback for macOS and improved zstd handling.

## [0.0.7] - 2026-05-22

### Changed

- CI cleanup: removed ccache setup and keyed the ccache on `matrix.os`.

## [0.0.6] - 2026-05-21

Large feature release: the async core, indexing, aggregation, query, and server
capabilities that define the current engine landed here.

### Added

- **Query DSL** for filtering trace events, wired into raw byte reading with
  file-level chunk skipping.
- **HTTP server** (from-scratch async) exposing trace query APIs, with streaming
  iovec-based responses.
- **Arrow data interchange** via nanoarrow (Arrow C Data Interface), zero-copy across
  the Python boundary.
- **Python runtime and bindings**: `Runtime` with async `submit()`/`TaskHandle` and
  Python-callable support, streaming iterators, a `StreamingUtility` with Arrow output,
  the Indexer wired to the Runtime, and a Dask plugin.
- **Aggregator** (`dftracer_aggregator`) with profile and system-event aggregation,
  custom metrics, offset tracking, and time-bucket persistence.
- **Comparator** (`dftracer_comparator`) for comparing trace metrics, injecting trace
  metadata into root SUMMARY rows.
- Bloom-filter multi-index, manifest (`.midx`) sidecars, a DFT view system with bloom
  filtering and predicate support, and an `index_threshold` to skip bloom/manifest for
  small files.
- Parallel statistics with DDSketch and log2 histograms.
- Replay (`dftracer replay`) with provenance-based, semantic trace reorganization.
- Call-tree utility with MPI support.
- DLIO config generation.
- C++20 coroutine core: async I/O backends (io_uring/kqueue), pipelines, task system,
  heterogeneous `when_all`/`when_any`, and channels.

### Changed

- **Breaking:** utilities migrated to the query DSL; core string/memory optimizations
  and a unified index infrastructure.
- Index storage migrated from SQLite to RocksDB.
- Replaced the `nonstd::span` polyfill with C++20 `std::span`; removed the xxhash
  dependency in favor of FNV-1a and standardized hashing.
- Broad performance work across parsing, scanning, serialization, compression,
  zero-copy I/O, and the aggregation pipeline.

### Fixed

- Coroutine foundation rewrite fixing `channel`/`when_all` memory leaks, `when_all`
  await-ready races, and channel lifetime management.
- gzip inflater trailer/window-reset handling and double-counting when an index
  already exists.
- Type-safety, lifetime, concurrency, and portability issues; endianness and query
  fallback handling.

## [0.0.5] - 2025-10-21

### Fixed

- Wheels now link their libraries correctly.
- Promise fulfillment and exception handling in pipeline executors.
- Performance when continuing to read from a gzip stream; default checkpoint size now
  comes from a defined constant.
- Dropped a soon-to-be-deprecated macOS version from the build matrix.

## [0.0.4] - 2025-10-20

### Fixed

- Race condition from mutating shared state.
- Source distribution now excludes unnecessary files and includes `cmake`/`tests`;
  corrected CLI options in the docs.

## [0.0.3] - 2025-10-20

### Fixed

- Corrected the versioning scheme and the link to the DFTracer GitHub repo.
- Excluded unnecessary files from the source distribution and fixed build scripts for
  the non-publishing path.

## [0.0.2] - 2025-10-19

### Fixed

- Added `setup.py` to fix a Python versioning issue and shipped missing scripts needed
  for Python publishing.

## [0.0.1] - 2025-10-19

Initial release.

### Added

- Core trace tooling: `dftracer_index` (gzip and tar indexers), `dftracer_merge`,
  `dftracer_split` (with `--verify`), `dftracer_event_count`, `dftracer_info`, and
  `pgzip`.
- A coroutine-driven task scheduler, executor, and pipeline framework with progress
  callbacks and multiple-output support.
- gzip block-boundary-aware reading with prefix-based hashes and checkpointing.
- Python packaging and Sphinx-based documentation with C++ API reference.
