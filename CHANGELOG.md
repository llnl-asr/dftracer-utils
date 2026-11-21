# Changelog

All notable changes to dftracer-utils are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with rules from [Common Changelog](https://common-changelog.org/), and this project adheres to [Semantic Versioning](https://semver.org/).

<!-- changelog-body -->

## [Unreleased]

### Added

- `TraceViewer` and `Indexer` take every input form through one C++ expansion (`runtime::fs::expand_inputs`): a file, a directory, a list of files, a list of directories, a list that mixes them, and globs with `*`, `?`, `[...]` (`!` or `^` negates) and `**` (any depth, including none), alone or in a list. A directory inside a list no longer raises `ValueError`. The files are those of the inputs in order, each once by device and inode when there are several inputs (a symlink or hard link names its file once); one directory or glob lists its files sorted by path. A scan or glob skips `.<name>.tmp-<id>` and `.<name>.old-<id>` staging folders and files, and `.dftindex` folders. A path that exists is taken as it is even if its name has a wildcard, a glob with no match is no files, and a missing path with no wildcard still raises `DFTUtilsNotFoundError` or at `collect()`. `Indexer` now scans a directory recursively (it listed only the files directly inside it before). C++: `dstd::fs::glob_match_name`, `has_glob_chars` and `trace::views::View::from_inputs`.
- Add the per-format write verbs `LazyFrame.sink_trace(path, *, options=None, mode="error", part_size=0, partition_by=None, max_open_files=64, max_partitions=1024, lazy=False)`, the same on `TraceViewer`, `sink_table(...)` with `name=None` and `meta=None` besides, and `DataFrame.write_trace(...)` and `DataFrame.write_table(...)` (eager, no `lazy`), which write a plan or a frame as DFTracer files and return `SinkSummary(root, rows, kind, part_count)`. `sink_trace` and `write_trace` write DFTracer events and raise a `ValueError` that names the missing columns when the plan lacks the event envelope; `sink_table` and `write_table` write any plan as typed `dftu.table` records. `Session.sink_trace`, `Session.sink_table` and `Session.sink` run them with the session. The sink verbs take `lazy=True` and return a `LazyResult[SinkSummary]` for `collect_all`. There is no `rows=` argument: the verb chooses the kind. The generic `LazyFrame.sink(path, *, format, ...)`, `TraceViewer.sink` and `DataFrame.write(path, *, format, ...)` are for plugin formats only and need `format`; `sink(path)` and `write(path)` without it, and `format="dftracer"`, are removed. A path ending in `.pfw.gz` or `.pfw` is one file; any other path is a folder of `part-NNNNN.pfw.gz` parts of `part_size` rows (plain output is one `.pfw` file; a folder with `compress=False` raises). The events keep the event envelope (`name`, `cat`, `pid`, `tid`, `ts`, `ph`, `dur`, `args.<key>`). One C++ writer compresses members in parallel under a byte budget, stages the output as `.<name>.tmp-<id>`, fsyncs, renames it into place and fsyncs the parent, so a failed or killed write leaves nothing at the target. `mode="error"` refuses an existing target and `mode="overwrite"` replaces it; the next write removes leftover staging paths of its target. A records header is its own gzip member of stored deflate blocks with `nrows` in a 20-character slot; a streamed sink writes `-1` and patches the slot and the member CRC32 before the commit, and readers refuse `-1`. One index is built for the output (`TraceWriteOptions(compress=None, level=6, member_size=0, build_index=None)`); the rows-complete flag is set for records. `format="<plugin sink>"` replaces `LazyFrame.sink(name, **options)`; the shared arguments that the caller sets (`name`, `meta`, `mode`, `part_size`, `max_open_files`) go to the plugin sink in its options under their own names, and a key given both ways raises. `mode="error"` renames the staging path without replacing, so a target created during the write is refused and left untouched; a folder overwrite that fails after moving the old folder aside restores it. A staging path of a process that is still running is never swept.
- Write Hive partitions with `partition_by=[...]` on `sink_trace`, `sink_table`, `write_trace` and `write_table`: each row goes to a `key=value/` folder (nested for several keys) of the output folder, with part files named inside it as without partitions. Names and values are percent-encoded (every byte but `A-Z a-z 0-9 . _ - ~`, so `/`, `=`, `%`, `:`, spaces, control bytes and non-ASCII bytes become `%XX`); a null key is `__HIVE_DEFAULT_PARTITION__`, and a literal value equal to it has its first byte encoded so the two never share a folder. A key column stays in each part, so every part reads alone. More than `max_partitions` (1024) distinct keys raise at the first excess key (`at least 1025`), and a folder name over 255 bytes raises before anything is committed. Rows wait in per-partition buffers under a quarter of the memory budget and the largest buffer becomes a gzip member when the buffers reach it; members compress in parallel with bounded bytes in flight and are appended to their part file in order; at most `max_open_files` part files are open, and a part closed to stay under it is reopened for more rows. The whole folder is committed by one rename, with one shared index at its root, and `TraceViewer(folder)` prunes with that index. A plugin sink still refuses `partition_by`.
- An index that cannot be written after a partitioned or fed write no longer fails the write: the data is committed, a warning names the index error, and the files are listed in `OutputSummary::unindexed`. Partition keys whose folder names differ only in case raise before anything is committed, naming both folders, because they are one folder on a case-insensitive file system. `max_partitions` reaches the write on every verb.
- Idle freed memory of the page heap is bounded across all threads (32 MiB backed in total, purged once it has sat idle for 10 ms), and on macOS a purge frees the pages at once, so the peak of a sink no longer grows by several MiB per worker thread.
- Index a table while it is written: the writer feeds each gzip member it forms to the index builder, which parses and folds it on a stage of its own under a byte budget while the next members compress, so the output is not inflated and parsed again, and the index is committed after the data rename under the final paths. The rows-complete flag is set and the index prunes as before. A trace output is still indexed after the rename, because its aggregation tier reads the committed file.
- Add the duql sinks `sink trace "path" key "value" ...` and `sink table "path" key "value" ...`, which write the rows of a pipeline as a DFTracer trace or a `dftu.table` file with the options of the Python verbs (`mode`, `part_size`, `partition_by` as comma separated names, `max_open_files`, `compress`, `level`, `member_size`, `build_index`, and `name` and `meta` for a table); `sink "name" ...` stays the plugin sink. The builder has `Pipe.sink_trace(path, **options)` and `Pipe.sink_table(path, **options)`.
- `DataFrame.from_pandas` carries the `dftu.table` header its pandas dtypes describe, built with no Arrow conversion: int and uint widths, `float32` and `float64`, the nullable `Int*`, `UInt*`, `Float*` and `boolean`, `string` and Arrow-backed dtypes, `object` strings, sets and lists, and categories with their labels and order. `write_table` stores them and `to_pandas()` gives the frame back until a plan changes it; a frame with a dtype a table cannot describe, such as a datetime, carries no header and is written from its schema.
- Add `dftracer.utils.concat(plans)`, which joins lazy plans, frames and trace viewers into one lazy plan (UNION ALL).
- Add `table: 1.0` to `VERSION` (`DFTRACER_UTILS_TABLE_FORMAT_VERSION_MAJOR` and `_MINOR` in `common/versions.h`, `dftracer.utils.dftracer_utils_ext.TABLE_FORMAT_VERSION`). The writer stamps `"format_version": "1.0"`; the reader takes a file with the engine's major and a minor not above its own, and the integer `1` that DFAnalyzer writes as `1.0`, and refuses any other value with the file's value and the engine's version.
- Read typed table files through `TraceViewer`. A file whose line 1 has `ph` 4, `name` "dftu.table" and `args.format` "dftu.table" is a table: `TraceViewer(inputs)` reads a table, a folder of parts or a list of them (the first gzip member of each file is probed with bounded concurrency), and tables and traces in one set raise, naming one file of each. A line 1 that claims the format but is malformed, or has an unknown `format_version`, raises and is never read as a trace. A table takes `filter` (an expression), `select`, `with_columns`, `drop`, `rename`, `sort_by`, `head`, `slice`, `group_by`, `agg`, `collect`, `lazy`, `schema` and `columns`; `phase`, `time_range`, `time_bucket`, `flamegraph`, `call_tree`, `containment`, `index_records`, `statistics` and the other trace ops raise `DFTUtilsValueError`. Parts must agree on name, dtype, ordered, elem, arrow_type and index names, and ordered categories on their labels and order, or the viewer raises before a row is read; unordered categories unify in first-seen order, and parts read in part-number order (`part-2` before `part-10`). One decoder serves every read: an unfiltered read with no index is the direct parallel decode, a filter without an index decodes members in bounded batches and decodes only the columns the plan needs, and a filter over a fresh index (mtime, size, schema) reads only the chunks the index cannot rule out (min and max, nulls and blooms) and falls back to a scan for a stale or missing index, a filter it cannot prove, or an index that does not record that the Indexer read every row of the table clean (an index built before that record existed, or a damaged table: a damaged row then raises as it does with no index). `Indexer` records that fact for each table file when it indexes it. `collect()` holds the whole result and returns a frame whose header is made for it, so `to_pandas()` stays exact; a derived frame has no header. `Indexer` indexes existing table files, so files written without an index and DFAnalyzer checkpoints are indexable offline.
- Add `dftracer.utils.trace.TraceWriter(path, *, level=6, threads=0, member_size=4 MiB, compress=True)`, a context manager that writes DFTracer records to a multi-member gzip or plain file: `write_event`, `write_counter`, `write_aggregated`, `write_metadata` (with `ph` as its integer) and `write_raw`. `args` takes `None`, `bool`, `int`, `float`, `str` and nested lists and dicts; NaN and infinities go out as the strings `"NaN"`, `"Infinity"` and `"-Infinity"`. C++: `trace::writer::TraceWriter`, which `dftracer_gen_fake_trace` now uses; its events put `type` before `pid`, as DFTracer does, and its metadata lines carry `pid`, `tid` and `ts`.
- An index whose aggregation tier has percentile sketches now serves a request with `compute_percentiles=False` as it is. Before, that request cleared and rebuilt the tier without the sketches, for every trace folder that shares the index root.
- Add `dftracer.utils.trace.discover_trace_files(path, recursive=False)`, which lists the trace files the indexer reads under a directory, glob or file.
- Add `DaskTraceViewer.index_record_futures(map_fn=None, progress=None)`: the distributed index records read as one ungathered future per worker, each holding an `IndexRecords`, with an optional worker-side `map_fn` and a progress phase for `ProgressAggregator`.
- Add `dftracer.utils.runtime.memory_budget_advice(required_bytes, available_bytes=0)`, which returns a typed `MemoryBudgetAdvice` (`fits`, `peak_bytes`, `suggested_nodes`, `warning`, ...). The untyped `memory_budget_advice` of the native module is now private.
- Add `dftracer.utils.runtime.worker_share(workers)` and `apply_worker_share(workers)` (C++ `common::worker_share`) so several worker processes on one host split it: each worker gets the usable CPUs divided by the worker count for its compute and I/O threads, and its share of the memory budget.
  `worker_share` returns a frozen `WorkerShare` (`threads`, `io_threads`, `memory_budget`, `env` with `DFTRACER_UTILS_THREADS`, `DFTRACER_UTILS_IO_THREADS` and `DFTRACER_UTILS_MEM_BUDGET`); `apply_worker_share` is the initializer for a `ProcessPoolExecutor` or `multiprocessing.Pool` and also applies the share to a runtime and a memory budget that already exist.
  The Dask helpers apply it to the workers of each host the first time they use a client.
- Read every calendar kernel of a zone-aware Timestamp column on the wall time of its zone, and add `tz_localize` for named zones.
  `dt` parts, `strftime` (`%z` the offset, `%Z` the abbreviation), `floor`, `ceil`, `round`, `normalize`, `date` and `time` read the wall time; `resample`, `asfreq`, `at_time` and `between_time` run on it (a day of a zone with daylight saving time is 23 or 25 hours, as pandas) and no longer raise `NotImplementedError` for a named zone.
  `Series.dt.tz_localize`, the index `tz_localize` and `Expr.dt.tz_localize` take pandas' `ambiguous` (`"raise"`, `"infer"`, `"NaT"`, a boolean array) and `nonexistent` (`"raise"`, `"shift_forward"`, `"shift_backward"`, `"NaT"`, a `timedelta`); `dt.replace_time_zone` and `dt.convert_time_zone` are the polars names (`ambiguous` raise, earliest, latest or null; `non_existent` raise or null).
  An unknown zone is an error (`KeyError` as pandas) where it read as UTC; the Python package gives the engine `zoneinfo.TZPATH` and the `tzdata` package's folder.
  C ABI: `dftu_series_tz_localize` (wall time to UTC, with `dftu_tz_ambiguous` and `dftu_tz_nonexistent`), the column op `dftu.series.tz_localize` (`dftu_series_tz_localize_opts`), and `dftu_tz_take_error` for the kind of a zone failure; `dftu_series_with_timezone` refuses a zone that is not in the database. `format_time` takes the offset and abbreviation of a zone.
  Two differences from pandas: `shift_forward` and `shift_backward` give the instant of the change, not pandas' whole-hour rounding, and `"+0530"` is +05:30 (pandas reads +05:00).
- Give the pandas names the pandas result shapes on the row index.
  A row-selecting op of a frame with no index keeps the original positions as an Int64 index, as pandas does: `dropna`, `drop_duplicates`, `sort_values`, `nlargest`, `nsmallest`, `tail`, `take`, `sample`, `query`, `df[mask]`, and the rows `loc` and `iloc` pick; an op that keeps every row in order makes no index, and the Polars names (`drop_nulls`, `unique`, `sort`, `top_k`, `gather`, `filter`) and `LazyFrame` stay positional.
  `df.loc[label]` of a label one row holds is a `Series` over the columns (the values of one type; a row of text and numbers is a `Row`), `df.loc[label, column]` is the value, and a repeated label gives its rows.
  `df.loc[new] = values`, `df.loc[new, column] = value`, `s[new] = value` and `s.loc[new] = value` add a row for a label the index does not have (pandas' setting with enlargement); a `Series` rebinds to the longer column through the new native `Series._replace`, which is a `BufferError` while a numpy array or memoryview over its buffer is alive, and setting a label the Series has is still a `TypeError`.
  An op whose index rule drops the index now drops its label columns too, eager and `LazyFrame`, so a hidden `__index_level_0__` no longer shows after `sort_values(...).merge(...)`: `join`, `with_row_index`, `null_count`, `describe`, `compare_agg`, `gap_fill` and a `concat` of an indexed with an unindexed frame. `compare_agg` and `gap_fill` drop only the hidden default labels, so a column you made the index with `set_index` stays a column; a `LazyFrame.compare_agg` drops the key index as the eager op does.
  A Series repr shows its labels, `groupby(c)["a"].nunique()` counts 0 for a group of only nulls, `DataFrame.query` takes the duql language (no `@var`, no backticks), and `dropna` is one pass.
  `groupby(keys)["column"]` is a `SeriesGroupBy`: its reductions, `agg` with one name and its transforms return a `Series` named by the column (a frame for `[["column"]]`, a list of names or `as_index=False`).
  `groupby(...).rolling(n)`, `expanding()` and `ewm(alpha)` return the rows in group order under the `(key, row)` index, as pandas; the Polars `group_by` keeps the input order.
  `Series.nlargest` and `nsmallest` take `keep` (`"first"`, `"last"`, `"all"`), a negative `tail(n)` is all but the first `-n` rows, `Series.compare` is indexed by the labels of the rows that differ.
- Add time on the row index of a `DataFrame` and a `Series`, as pandas 2 and 3 have it: `resample(rule, closed, label, on, level, origin, offset)` (fixed widths and the `W`, `ME`, `MS`, `QE`, `QS`, `YE`, `YS` rules, empty bins listed, `sum`, `prod`, `mean`, `median`, `min`, `max`, `count`, `size`, `first`, `last`, `std`, `var`, `nunique`, `ohlc`, `agg`), `asfreq(freq, method, how, normalize, fill_value)`, `at_time`, `between_time(inclusive=)`, `tz_localize` and `tz_convert` of a Timestamp index level (`level=` for a MultiIndex; a non-Timestamp index is a `TypeError`).
  They are Python over the existing kernels (bin arithmetic, `cut`, group-by, the label reindex), so no C++ or ABI change.
  `tz_localize` and `tz_convert` now act on the index, as pandas does, and no longer take `on=`; `at_time`, `between_time` and `asfreq` no longer take `on=` (set the column as the index); `DataFrame.resample` takes the pandas arguments in the pandas order (`unit` is keyword only), and a frame with no Timestamp index is a `TypeError` where it was a `ValueError`. An Int64 index or `on=` column keeps the old fixed-width form.
- Give the `DataFrame` a native row index: `DataFrame::index_names()`, `with_index(names)` and `without_index()` (C++), `dftu_frame_index_count`, `dftu_frame_index_name` and `dftu_frame_with_index` (C ABI) and `index_names()` / `with_index(names)` on the Python `DataFrame` and `LazyFrame`.
  The index is an ordered list of column names that the frame holds behind one pointer, so a frame with no index pays 8 bytes; the columns stay ordinary columns.
  `frame_op_index_rules.def` gives every frame op a rule, and a ctest fails on an op without one: row ops, projections and column-adding ops keep the names whose columns survive, `rename`, `group_by`, `group_by_dynamic`, `pivot`, `join`, `concat` and `union` rebuild them (only for an indexed input), and `describe`, `reduce`, `with_row_index`, `unpivot` (as pandas `melt` gives a RangeIndex) and the summaries drop them.
  A lazy plan applies the same rules, and `LazyFrame.set_index` declares the index on the plan.
  Arrow IPC, the C Data Interface and the C Stream Interface carry the names in the schema metadata key `dftracer.utils.index`, and an Arrow schema with pandas metadata maps its `index_columns` to them.
  Python `set_index`, `reset_index`, `index` and `index_levels` read and write the native field in place of the Python-only attribute, `to_pandas()` moves the index columns into the pandas index and `from_pandas` records a non-`RangeIndex`.
  `select` of some of the index columns now keeps the remaining names, where the old attribute was dropped unless every index column survived.
  A `DataFrame` with an index shows as pandas shows it: `columns`, `dtypes`, `shape`, `keys()`, `items()`, `to_numpy()`, `df[list]`, the reducers and the positions of `iloc` leave out the index columns, and the Polars names (`schema`, `select`, `filter`, `with_columns`, `group_by`, `col`) still see them; a frame with no index changes in nothing.
  The pandas `groupby(...)` has `as_index` (default `True`): an aggregate result is indexed by the keys, as pandas does, even for a frame with no index; `as_index=False` and the Polars `group_by` keep the keys as columns.
  The pandas-named `describe`, `drop_duplicates`, `dropna`, `fillna`, `to_dict`, `to_csv`, `to_string`, `to_markdown`, `to_html` and `drop(columns=...)` read only the visible columns and keep the index columns in step with the rows; `drop(columns=[index column])` raises `KeyError`, and `unique`, `drop_nulls`, `fill_null` and `drop("k")` still see every column.
  `get_group` and a native frame's bytes carry the index too (the bytes only when there is one).
- Give the `Series` native row labels: a Series holds optional label columns behind one shared pointer (`Series::index_count()`, `index_name(i)`, `index_level(i)`, `with_index(names, levels)`, `without_index()`, `index_frame()` in C++; `dftu_series_index_count`, `dftu_series_index_name`, `dftu_series_index_column` and `dftu_series_with_index` in the C ABI), so an unlabelled column pays 16 bytes and the layout budget of `dftu_series` rises from 152 to 168.
  `df["col"]` of a frame with an index is a Series under the frame's labels; the ops that keep rows keep the labels, `sort`, `filter`, `take`, `head`, `tail`, `slice`, `drop_nulls`, `top_k`, `bottom_k`, `sample` and `reverse` move them with the rows, and reducers and `unique` give none.
  Arithmetic and logical ops between two Series align on the labels as pandas does (`align_labels`, `same_labels`: the outer join of the labels, sorted, a null for a missing row, an integer column that gains a null widens to Float64, a Bool one takes false, a null label equals a null label, numeric labels of different types meet in their common type); comparing two Series with different labels raises `ValueError`.
  `Series.sort_index` orders a labelled Series by its labels; `s[mask]` keeps the rows where a Bool Series is true.
  Python `Series.index`, `axes`, `index_names()`, `set_axis`, `rename_axis`, `reset_index(level=, drop=, name=)` and `to_frame` use the labels, `Series.from_pandas` and `to_pandas()` carry a non-default pandas index (a `MultiIndex` for several levels), and the pandas names `sort_values`, `nlargest`, `nsmallest`, `tail`, `take`, `dropna`, `drop_duplicates`, `repeat` and `truncate` label a positional Series by the rows they keep.
  `truncate` of a labelled Series cuts by label on a sorted index.
  A group-by `apply` that returns a Series keeps the original labels of the rows.
  The `[index]` parity findings of the Series row methods and the `Series.axes`, `index`, `rename_axis` and `set_axis` coverage exclusions are gone.
- Look rows up by label through a hash built once: `Series::locate(keys)` and `DataFrame::locate(keys)` (C++) give the row positions of label keys, one key column per level (a leading part of them for a partial MultiIndex key), with the keys no label matches listed apart, and `locate_range(start, stop)` gives the rows between two label bounds.
  The first lookup hashes the labels natively (a canonical cell per label, so `1.0` finds the label `1`, a null or NaN key finds the null labels, a text finds no number; repeated labels keep their rows in order) and keeps the hash with the labels: a Series holds it behind its label pointer, a frame behind its row index (still one pointer), copies share it, it is built once even when several threads look up together, and it is freed with them. A lookup on data other than the hash was built from builds its own and keeps nothing.
  A range on an increasing or decreasing index (lexicographic for several levels, no null label) is a binary search and its bounds need not be labels; on an unsorted index both bounds must be labels no other row has (as pandas), and an unsorted MultiIndex is refused.
  Python `loc`, `at`, `xs`, `get` and `s[label]` on a DataFrame and a Series use it in place of comparing the whole index column per lookup: a scalar label, a list in its own order with repeats expanded, a Bool mask, an inclusive label slice (sorted by binary search, unsorted as pandas), a MultiIndex whole or partial key (a partial key drops the levels it names), `xs(key, level=, drop_level=)`, and a `KeyError` naming a missing label; a Series with no labels has the positions as labels.
  `s[label]` follows pandas 3 (an integer is a label on a Series with labels); a `loc` slice on a MultiIndex sorted by its levels works where it raised `TypeError`; a Series of labels as the `loc` key and the list form give the rows in the order of the labels, where the old mask kept the frame's order.
  The `loc` of a `LazyFrame` still builds a mask over the index columns. The C ABI is unchanged: no plugin needs a label lookup.
- Reindex and align by row label, natively: `Series::reindex(keys, options, fill)` and `DataFrame::reindex` (C++; `ReindexOptions` and `FillMethod` carry the pandas `method`, `limit` and `tolerance`) give the rows of the label keys through the label hash, a null (an integer column widens to Float64) or a fill value that keeps the column type when it fits for a label with no row; `align_labels(a, b, how)` and `align_rows(frameA, frameB, how, fill)` align two Series or two frames on the inner, left, right or outer join of their labels (outer sorted, nulls last; a repeated label meets every row that has it).
  Python `Series.reindex`, `DataFrame.reindex` (`labels`, `index=`, `columns=`, `axis=`, `method=`, `fill_value=`, `limit=`, `tolerance=`), `reindex_like` and `align` (`join=`, `axis=`, `fill_value=`, a Series against a frame too) follow pandas 2 and 3: `ffill`, `bfill` and `nearest` on increasing or decreasing labels by binary search, `ValueError` for repeated labels (unless the keys equal them), a non-monotonic index with a method, and `limit` or `tolerance` with no method.
  Not supported: `level=`, a fill method with `columns=`, a tolerance per label, a scalar key on a MultiIndex, a Series with several index levels against columns.
- Edit the levels of a row index on a Series and a DataFrame, as pandas 2 and 3: `droplevel`, `swaplevel`, `reorder_levels`, `rename_axis` with a list, a dict or a function, `add_prefix` and `add_suffix` on the row labels (and `axis=0` on a frame), `DataFrame.set_index(append=True)` (also on a `LazyFrame`), `reset_index(level=)`, `sort_index(level=, sort_remaining=, ascending=, na_position=, ignore_index=, key=)`, and `names`, `nlevels`, `get_level_values` and `set_names` on what `index` returns.
  The group-by results that pandas labels with the group keys in front are labelled the same way by the pandas `groupby`: `apply` that returns a frame or a Series, `take`, `SeriesGroupBy.nlargest` and `nsmallest` and `value_counts` lead with the key levels, then the label each row had (or its position).
  Not supported: a level name that is also a column (a frame holds no duplicate names, a `ValueError`), the name of the column axis, and `level=` of `reset_index` on a `LazyFrame`.
- Reshape by row label, as pandas 2 and 3: `DataFrame.stack(level=-1, dropna=, sort=, future_stack=True)`, `DataFrame.unstack(level=-1, fill_value=None, sort=True)` and `Series.unstack(level=-1, fill_value=None, sort=True)`.
  `stack` makes the columns the innermost index level of a Series, row by row, over the native unpivot and one take; the default is pandas 3's implementation (every cell kept, `dropna` and `sort` refused), and `future_stack=False` drops null cells unless `dropna=False`. The columns must share a type or all be numeric.
  `unstack` moves a level (a name, a position, a negative position or a list) into the columns over a sort that numbers the rows and the columns and the native pivot: the other levels are the sorted index, a missing cell is null or `fill_value` (a value the Series holds as null stays null), `sort=False` keeps the order of first appearance, and a label that occurs twice is a `ValueError` ("Index contains duplicate entries, cannot reshape"). A `Series.unstack` names the columns with the label text; a `DataFrame.unstack` with a MultiIndex names them with the text of the tuple `(column, label)`, as `from_pandas` names a MultiIndex column, and several levels in `level` give tuples in the order given. A frame with one index level (or none), or with every level named, gives a Series labelled by the column and the old labels.
  Not supported: a column level name (the result's column axis is unnamed), a mixed text and number frame for `stack` and a flat `unstack` (pandas gives an object Series), and a fill value of the other kind than the column (a text fill for text, a bool for Bool, a number for the rest; a number fill on a Bool column raises where pandas mixes types). The Polars `DataFrame.unstack(step, ...)` form is not offered: the name is the pandas method. `Series.unstack(sort=False)` follows pandas 3.1; pandas before 3.1 returns values under the wrong labels.
- Gather rows by a native Int64 Series: `Series.take`, `Series.gather`, `DataFrame.take` and `DataFrame.gather` read the buffer of an Int64 Series without nulls in place, where they turned it into a Python list first (a 1M-row take is about 85 ms faster).
- Choose the summaries of single paths, list what an index stores, and hash a value as the index does.
  `Indexer(summaries={"args.fhash": ["bloom"], "size": ["zonemap"]})` (C++ `IndexerOptions::summaries`) indexes a path with only the summaries named ("zonemap", "bloom" or "counts") in place of the defaults; a path that is not a fixed field is indexed by name, and an empty or repeated path, an unknown summary or one outside `extensions` is an error that names it.
  The choice is a build input: a change rebuilds the zonemap, bloom and counts of the indexed files, and the same choice does not.
  `dftracer.utils.index.Index(status.index_dir).summaries(file)` and `Indexer.summaries(file)` (C++ `summary_rows`, `summary_frame`) return a DataFrame with one row per stored summary per chunk: `path`, `chunk`, `summary`, `bytes` and `hash_version`.
  `Indexer.explain` adds `summaries`, the chunks each summary of each filtered path skips when it judges only that path (C++ `FileExplain::summaries`), and `Indexer.explain_text` (C++ `to_text`) prints `summary <path> <kind>: skipped K of N chunks` for each.
  `@jit.summary(name=..., paths=[...])` compiles a Python class with `add(self, value)`, `merge(self, other)` and `may_contain(self, cond)` over `int` or `float` state fields to a native index extension that `summaries={"size": ["acme.size_range"]}` names; a query runs it on native threads with no Python code, and `explain` counts the chunks it skips like any other summary.
  `dftracer.utils.index.hash_value(x)` returns the canonical typed hash of `None`, a bool, an int, a float, a `str` or `bytes`, the value of the C++ `dstd::hash::value_hash_*` functions and of `dftu_svc_index.hash_value`.
- Add the Polars elementwise math methods `sin`, `cos`, `tan`, `cot`, `arcsin`, `arccos`, `arctan`, `sinh`, `cosh`, `tanh`, `arcsinh`, `arccosh`, `arctanh`, `cbrt`, `log10`, `erf` and `erfc` to `Series` and `Expr`, and the kernels `dftu_series_sin` to `dftu_series_erfc`.
  A Float32 column stays Float32 and every other numeric column gives Float64; a value outside the domain is NaN (`arcsin(2)`), `cot` is `1 / tan`, nulls stay null, and a Bool or text column is refused.
  `Expr.log10` is the `log10` kernel now, exact for a power of ten, where it was `log(x) / log(10)`.
- Add the Polars bitwise methods `bitwise_count_ones`, `bitwise_count_zeros`, `bitwise_leading_ones`, `bitwise_leading_zeros`, `bitwise_trailing_ones` and `bitwise_trailing_zeros` (a UInt32 column within the column's own width, a signed value as its two's complement bits, a Bool as one bit) and the reductions `bitwise_and`, `bitwise_or` and `bitwise_xor` to `Series` and `Expr`, with the kernels `dftu_series_bitwise_*`.
  The reductions give a scalar of the column's integer or Bool type, `None` for a column with no present value, and `Expr` forms run in `select`, `group_by().agg` and the lazy plan.
  They are the aggregates `AggOp::BitwiseAnd`, `BitwiseOr` and `BitwiseXor` / `Agg::BitwiseAnd`, `BitwiseOr` and `BitwiseXor` / `DFTU_AGG_BITWISE_AND`, `DFTU_AGG_BITWISE_OR` and `DFTU_AGG_BITWISE_XOR` ("bitwise_and", "bitwise_or", "bitwise_xor"): the result keeps the value column's type and is null for a group with no present value, where the older `BitOr` reads the column as u64 and gives 0.
  A float or text column is an error from `agg_accumulate`.
  Remove the Python `Expr.bit_or`: `Expr.bitwise_or` replaces it. The `AggOp::BitOr` row stays (the C ABI, the JIT monoids and `DataFrame.reduce("bit_or")` use it).
  A bitwise or math method on a column of another type raises `DFTUtilsValueError` with the type the method needs, and a bitwise aggregate on such a column is an `InvalidArgument` error, an all-null column included.
  The state is a 64-bit word and a flag per group, so partial states merge in any order.
- Load plugins for the whole process and pick every reader with one probe.
  `DFTRACER_UTILS_PLUGINS` names plugin paths separated by `:`; the CLI tools, the Python extension and the server load them once, at the first call that reads a trace, and a path that does not load is an error that names it.
  Python `dftracer.utils.plugins.load_plugin(path)` returns a handle that works as a context manager and unloads the plugin at `close()` or the end of the block; it refuses, with an error, while a plan made by `scan_file` through one of its formats is alive.
  The built-in trace reader (`jsonl`) joins the content probe with a score of 50 for a gzip file or text whose first lines are JSON objects, so `Indexer`, `TraceViewer` and `scan_file` pick a reader the same way: the highest score wins and a tie, now with the built-in reader too, is an error that names both.
  `format=` of `scan_file`, `TraceViewer` and `Indexer` (C++: `LazyFrame::scan_file(path, index_dir, format)`, `IndexerOptions::format`, `ViewFile::format`) and `--format` of `dftracer_index`, `dftracer_view` and `dftracer_run` force a reader by name, and an unknown name is an error that lists the known ones.
  `dftracer_index` indexes a file that a plugin file format claims through the Indexer.
  `scan_file` reads a gzip or JSON-lines trace through the built-in reader; before it was a `no format claims` error.
- Index the files a plugin file format reads.
  `dftu_file_format_vt` gets `chunk_token`, an optional callback that names each chunk of a file by its bytes (a byte offset, a row group number); it is the last field, so a plugin built before it loads unchanged.
  `Indexer` indexes a file that a registered format claims, one granule of the zonemap, bloom, counts, postings and plugin column extensions per chunk, and records the format name, the summary settings and the chunk tokens per file (`core.format`); a file whose size, mtime, format or settings changed is indexed again, and no other file or folder under the index root is.
  `LazyFrame::scan_file(path, index_dir)` skips the chunks the index proves hold no row that a conjunction of `column <, <=, >, >=, == literal` filters keeps, and reads every chunk when the index is missing, stale, another reader's or built over other chunks.
  A format without `chunk_token` is one granule: the index can rule out the file, not a chunk.
  `dftu_lazyframe_scan_file` takes `index_dir` and its length (NULL and 0 for the index beside the file), Python `scan_file(path, index_dir="")` takes it too, and a C++ format gives its chunks tokens with `using File = MyFile;` and `MyFile::chunk_token(chunk)`.
  `Indexer::open` on a directory still takes only trace-pattern files: name each format file.
  The index of a format file records the version of the plugin that registered the format, so a plugin of the same name at another version indexes its files again, and `Indexer::rebuild_extension` builds the named summary for a format file too.
- Show what a plugin plans in `explain`, and run a plan with `explain analyze`.
  `dftu_source_vt`, `dftu_file_format_vt` and `dftu_node_vt` get an optional `explain` callback at the end (a source and a format get the scan request, a node its arguments) that adds key and value lines through `dftu_explain_out`, and `dftu_ctx_vt` gets `counter` at the end, with the helpers `dftu_counter` and `dftu_counter_n`; a plugin built before them loads unchanged.
  `LazyFrame::explain()` and Python `explain()` print one line for each plugin source, file format (with the chunks its index keeps) and node that has a hook, and nothing for one that has none.
  `LazyFrame::explain_analyze()`, `dftu_lazyframe_explain_analyze`, Python `explain(analyze=True)` and the duql prefix `explain analyze` (`explain` alone gives the plan) run the plan, drop the rows and add the rows in and out and the time of each stage, the votes (`exact`, `inexact`, `no`) the source gave each filter, and the counters plugins added; the duql prefix returns the text as the rows of a column `plan`, and `Pipe.explain(analyze=True)` and `TraceViewer.explain(analyze=True)` build it.
  Each analysis makes its own counters and hands them to the scans and nodes it opens, so analyses at once, or beside a plain run, never mix; a call outside an analysis is a null check.
  The C++ SDK sets the hooks for a source, a format file and a node with an `explain(...)` method and adds `plugins::counter(name, delta)` and `ExplainOut`.
- Let a plugin plan a scan before it opens a cursor.
  `dftu_source_vt` and `dftu_file_format_vt` get an optional `plan` and `plan_duql` at the end, and `dftu_scan_request` gets `plan` at the end, so a plugin built before them loads unchanged and one without `plan` votes in `scan` as before.
  `plan` fills a `dftu_plan_result`: one vote per filter, a relative cost, an estimated row count and a token that `scan` reads back in `dftu_scan_request::plan`; the host decides which filters stay above the scan from the votes, before it scans, and ignores the votes `scan` writes.
  `plan_duql` gets the filters it can say as one duql condition (`dftu_plan_duql`) through the new filter-to-duql converter; the filters it cannot say stay in the request.
  A failing plan fails the query with the message of the plugin, and `explain` prints `plan_votes`, `plan_cost` and `plan_est_rows` for each plugin that has a plan.
  The C++ SDK sets the hooks for a source and a format file from `plan(const ScanView&)` and `plan_duql(...)` methods, with `PlanResult` and `ScanView::plan`.
- Let a plugin index extension take typed columns in place of record lines. `dftu_index_extension` gets `columns` (a NULL-ended list of paths, `{"*", NULL}` for every path) and `step_column`, which receives one borrowed column per path and chunk with the dtype int64, uint64, float64 or string; `step` is now optional, and an extension with neither is refused at registration with a message that names it.
  A path whose values have no single type in a chunk is not passed, and the host stores nothing for that chunk, so it is never pruned.
  `dftu_index_leaf` gets `type`, the dtype of the column the leaf targets, and `dftu_svc_index` gets `hash_version`, `hash_value` and `hash_series`, the host's canonical value hash for a literal and for a whole column.
  The fields are added at the ends of the structs, so a plugin built before this change loads and runs unchanged.
- Add `AggOp::ArgMaxPos` and `ArgMinPos` / `Agg::ArgMaxPos` and `ArgMinPos` / `DFTU_AGG_ARGMAX_POS` and `DFTU_AGG_ARGMIN_POS` ("arg_max", "arg_min"): the position within its group, from 0 and in input order, of the first greatest or least present value, an Int64 that is null for a group with no present value.
  The value is read exactly in its own domain (signed, unsigned or float; a NaN wins only in a group with no number, then the first NaN, as Polars) and a tie keeps the first row.
  The state is fixed size but counts the group's rows, so it accumulates in input order (a serial pass; the parallel lanes and the partitioned group-by decline it), partial states merge in row order and `agg_check` reports a partial that starts before the one it joins; a String column is refused.
  `Expr.arg_max()` and `Expr.arg_min()` (Polars) build it for `group_by().agg` and `select`; the trace-view aggregation refuses it.
- Add `Series.dt.as_unit(unit)` (pandas) and `Series.dt.cast_time_unit(time_unit)` (Polars), `Expr.dt.cast_time_unit`, and the kernel `dftu_series_dt_as_unit`: a Timestamp or Duration in another unit with the instant kept.
  A coarser unit floors toward negative infinity (`DFTU_DT_UNIT_FLOOR`, the pandas rule); `cast_time_unit` uses `DFTU_DT_UNIT_POLARS`, which floors a Timestamp and truncates a Duration toward zero as Polars does.
  A value that does not fit 64 bits in a finer unit is an error (`DFTUtilsValueError`), where Polars wraps.
- Add `Series.dt.date`, `dt.time` and `dt.timetz` (pandas) and `Expr.dt.date` and `Expr.dt.time` (Polars), and the kernels `dftu_series_dt_date` and `dftu_series_dt_time`: a Timestamp (or a Date64 for the date) to a Date32 column, and a Timestamp to its UTC time of day in its own unit (Time32 for seconds and milliseconds, Time64 for microseconds and nanoseconds).
  The day and the time are the wall day and time in the column's zone, as every `.dt` part; `timetz` is `time`, because a time-of-day column carries no zone.
- Add the pandas `SeriesGroupBy` methods `nlargest(n)`, `nsmallest(n)`, `unique()`, `is_monotonic_increasing` and `is_monotonic_decreasing`, and `DataFrameGroupBy.corrwith(other, drop, method, numeric_only)`; each runs on engine kernels (the group transforms, the sort, `unique`, the `corr`, `var` and `count_valid` aggregates), with no Python loop over rows.
  The results are frames with the keys, in key order, then the value column (`index` and the value for the top-n methods, a List column for `unique`, a Bool column for the monotonic checks); `nlargest` and `nsmallest` take `keep="first"` only, and `corrwith` pairs rows by position and takes `method="pearson"` only.
  The binding `_series_list_of(values, ends)` builds a List column from group end positions.
- Add the exact quantile aggregate, `AggOp::Quantile` / `Agg::Quantile` / `DFTU_AGG_QUANTILE` ("quantile"), with `agg_quantile` and `dftu_agg_quantile`: the group keeps every present value (8 bytes each), partial states merge by concatenation, the aggregation spill path bounds the state across groups and a group with no value is null.
  The level and the interpolation are separate fields (`AggSpec::interp`, `dftu_agg_spec::interp`); a level outside [0, 1] or NaN is an `InvalidArgument` error from `agg_quantile` and the engine, and `dftu_dataframe_group_agg_expr` returns -1 for it.
  `Expr.quantile(q, interpolation="nearest")` (Polars), `Expr.percentile(p, interpolation="linear")` and `Expr.median()` are exact; `Expr.approx_quantile(q)` is the DDSketch estimate they used to be.
  `GroupBy.quantile` and `LazyGroupBy.quantile` take `interpolation` and run on the aggregate.
  A level outside [0, 1] or NaN raises `ValueError`.
- Add `Series.log1p`, `Series.expm1` and the expression forms, with the kernels `dftu_series_log1p` and `dftu_series_expm1`; `Expr.log1p` was `log(x + 1)` and lost a tiny `x`.
- Add `Expr.pow`, `floordiv`, `mod` and the operators `**`, `//` and `%` over the Series kernels, and `Expr.median`, `n_unique`, `len` and `null_count` aggregates.
- Add the Polars names `select_seq`, `with_columns_seq` (frames and plans), `DataFrame.is_sorted`, `iter_slices`, `remove`, `bottom_k`, `LazyFrame.top_k`, `bottom_k`, `shift`, `cast`, `gather_every`, `remove`, `Series.not_`, `clear`, `radians`, `degrees`, `Expr.radians`, `degrees`, `pipe`, `Series.dt.total_days` to `total_nanoseconds` (whole units, truncated toward zero, in integer arithmetic), `Series.dt.timestamp`, `to_string`, `min`, `max`, `Series.str.head`, `tail`, and `Expr.str.head`, `tail`, `Expr.dt.timestamp`, `to_string`.
- `scripts/api_coverage.py` rejects a `needs-kernel` line whose detail points to a method we already have.
- Add `scripts/api_coverage.py`, a gate that lists the public names of pandas (`DataFrame`, `Series`, both group-bys, `.str`, `.dt`) and Polars (`DataFrame`, `Series`, `LazyFrame`, `Expr`, both group-bys, the `str`, `dt` and `list` namespaces), subtracts ours and the reasoned list in `scripts/api_coverage_exclude.txt`, and fails on a name that is neither; `--report` prints the table.
  A line for a name that is now ours fails too, so the list only shrinks.
  `tests/python/test_pandas_parity.py` and `tests/python/test_polars_parity.py` run each pandas or Polars method that we have on the same data in the library and in ours and compare type, shape, dtype, values and, for pandas, that the result has the default RangeIndex; `tests/python/parity_findings_pandas.txt` and `parity_findings_polars.txt` list every method whose result differs, with its reason.
- Add `Expr.lt`, `le`, `gt`, `ge`, `add`, `sub`, `mul`, `truediv`, `neg`, `and_`, `or_` and `not_`, the method forms of the operators.
- Add the Polars spellings `Series.alias`, `clone`, `limit`, `has_nulls`, `is_empty`, `first`, `last` and `is_not_nan`, `DataFrame.clone`, `clear`, `iter_columns` and `collect_schema`, and `LazyFrame.dtypes`, `width`, `collect_schema`, `lazy`, `clone`, `clear`, `first`, `last` and `pipe`.
- Add the pandas spellings `Series.squeeze`, `T`, `transpose`, `dtypes`, `convert_dtypes`, `infer_objects`, `Series.dt.day_name` and `month_name`, and `DataFrame.convert_dtypes` and `infer_objects`.
  `Series.diff` takes `n` (or `periods`) for a gap other than one row, and `DataFrame.drop` takes `axis=1` (or `"columns"`); `axis=0` raises, because a frame has no row labels yet.

- Add `nulls_last` to the engine sort: `Series.sort` and `argsort`, `DataFrame` and `LazyFrame` `sort_by` and `sort_by_multi` (one flag, or one per key), the registry rows and the C functions `dftu_series_sort`, `dftu_series_argsort`, `dftu_dataframe_sort_by`, `dftu_dataframe_sort_by_multi`, `dftu_dataframe_sort_by_multi_per_col`, `dftu_lazyframe_sort_by` and `dftu_lazyframe_sort_by_multi`.
  Every mode is stable, also for equal values and for nulls first with `descending=True`, in memory and in the external merge sort, so Python no longer sorts over a presence key and `Expr.arg_sort` no longer reverses the opposite sort.
  A lazy sort with nulls first is not pushed down into a source.
- Add `maintain_order` to `Series.unique`: the distinct values with one null kept, sorted with the null first or in order of first occurrence.
  Add `dftu_series_duplicated(v, keep)` and `Series.duplicated(keep)`, and `side` to `Series.search_sorted`, which are native; `dftu_series_unique` and `dftu_series_search_sorted` take the new argument.
- Add the aggregates `SkewUnbiased` and `KurtUnbiased` (`skew_unbiased`, `kurt_unbiased`, `DFTU_AGG_SKEW_UNBIASED`, `DFTU_AGG_KURT_UNBIASED`) and the series reducers `dftu_series_skewness_unbiased` and `dftu_series_kurtosis_unbiased`: the pandas sample skewness and excess kurtosis, from the same moment state as the population forms, so a merged or spilled state gives the same value.
  `Expr.skew` and `Expr.kurt` take `bias=False` for them, and `agg` takes the pandas names `skew` and `kurt` for them.
- Add the methods `Series.is_null_at(i)`, `Series.str.count_matches`, `strip_chars_start`, `strip_chars_end`, `strip_prefix`, `strip_suffix`, `to_titlecase`, `Series.dt.epoch`, `iso_year`, `ordinal_day`, `week`, `truncate`, and on `Expr.dt` `ceil`, `floor`, `normalize`, `total_seconds`, `tz_localize`, `tz_convert`, `dayofweek`, `dayofyear`, `days_in_month` and the `is_month_start` family.
- Add the native types `StrAccessor`, `DtAccessor` and `ListAccessor` behind `Series.str`, `Series.dt` and `Series.list`.
  The accessor methods that are exactly one kernel (`lower`, `strip`, `len_chars`, `startswith`, `find`, `replace_all`, `zfill`, `list.len`, `list.get` and the rest of the rows with class `Str` or `List` in `core/api.def`) are generated from that table and typed in the `.pyi` by `scripts/gen_pyi.py`; a wrong call count or keyword now gives the message of the native argument parser.
- Add the native types `Rolling` and `Ewm` behind `Series.rolling(window)` and `Series.ewm(...)`, and the native `Series.rolling_sum`, `rolling_mean`, `rolling_min` and `rolling_max`.
  A window checks its arguments when `rolling` or `ewm` is called, not when you call a method of the window, and a bad argument raises the error of the native call.
  `DataFrame.ewm` and `GroupBy.ewm` check `alpha`, `span`, `com` and `halflife` when they are called.
- Add a tutorial on the JIT plugin API, `docs/source/tutorials/extending-the-engine.rst`, with plain state, a batch hook, a config, a finalize task, a channel and a UDF, and run every example of it and of `docs/source/jit.rst` from `examples/jit/` in `tests/python/test_docs_examples_jit.py`.
- Add `@dft.udf` to register a Python function as a scalar UDF in two forms.
  A typed `def f(x: float, n: int) -> float` is compiled to a native UDF (per-row arithmetic, comparisons, conditions, `abs`, `min`, `max` and `math` functions) that expressions and duql call by name and that never takes the GIL; a body outside the subset is a located `JitError`, never a silent fallback.
  `@dft.udf(batch=True)` calls the function once per batch with read-only numpy views of the argument columns, on one dedicated Python lane thread that holds the GIL for the batch; workers queue a batch and wait without the GIL, and the engine releases the GIL while it waits for its workers.
  A wrong result length or dtype and an exception in the function fail the query with the UDF name, a queued batch fails as cancelled when the lane stops, and interpreter exit with an open UDF does not hang.
- Add `with dft.udf.scope():` and `Udf.close()`: a UDF that `@dft.udf` made stays registered until it is closed or its scope ends, never because it was garbage collected, and a name clash is an error that names both definitions.
  `dft.udf` stays the read-only mapping of UDFs.
- Release the GIL around `DataFrame.group_by(...).agg(...)` and `Expr.apply`, so an aggregate over a per-batch Python UDF no longer deadlocks, and keep a view that a per-batch UDF holds valid after the call.
- Add `dftracer::utils::comm` in `libdftracer_utils_comm`: a `Communicator` that makes `Group` handles (`world`, `local`, `split` with `NO_COLOR`), and every call returns an `Op<T>` that can be tested, waited for, cancelled or awaited, and ends exactly once.
  A group has `barrier`, `monitored_barrier` (names the ranks that did not arrive), `broadcast`, `gather`, `all_gather`, `scatter`, `reduce`, `all_reduce`, `reduce_scatter`, `all_to_all`, `exchange_by`, `scan`, `exclusive_scan` and `neighbors`, and a reduction folds in rank order, so a floating point result is the same on every run, unless `CallOpts::reduce_order` says `Any`.
  A channel (`Group::channel<T>(name, ChannelOpts)`) is a named stream with a depth, `Overflow::Block`, `Fail` or `DropOldest`, `recv_from`, `recv`, `recv_in_rank_order`, `recv_into` (a short buffer gives `ChannelError::TooBig` and keeps the message) and an ordered `close`.
  The `_object` calls move a value that is not trivially copyable through a `Codec`: ranks in one process share the object with no copy and no codec call, and a value is serialized once, only for a rank in another process.
  `expose`, `pull`, `push` and `unexpose` move bulk data with an access mode, a 64-bit size and credits from the memory budget, and never cut a range silently.
  A deadline, a cancel or a stopped token on a collective aborts the group on every rank with one error that names the rank, and on a channel call or a bulk transfer it ends that call alone.
  `Communicator::start(BackendKind)` runs on `Local`, `Mpi` or `Auto`, and `Communicator::start_local(ranks, executor)` runs every rank as a coroutine of the process.
- Add `FaultMessaging`, a messaging provider for tests that delays, reorders, drops or kills by seeded rule, so a killed rank fails every pending call of its groups with one error that names it.
- Add `Communicator::stats()` and `Communicator::pending()` for the counters of each group and kind of call and the collectives still waiting, and `DFTRACER_UTILS_COMM_TRACE`, a directory where each rank writes a DFTracer event file with one event for each call that ended.
- Load the MPI backend at run time from `libdftracer_utils_comm_mpi`, built with `-DDFTRACER_UTILS_ENABLE_MPI=ON`, so the tools no longer link MPI.
  `DFTRACER_UTILS_COMM_PATH` names the one directory that holds it, and `DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES` lowers the size of one MPI message.
  The backend runs the collectives of each group on the nonblocking collectives of MPI, routes the messages of groups made by `split` to the right processes, moves calls of any size in rounds of at most 1 GiB, and ends the job with `MPI_Abort` and a non-zero status when a native collective is never matched, in place of a hang.
- Add `--comm local|mpi|auto` and `DFTRACER_UTILS_COMM` to `dftracer_view`, `dftracer_genesis_gen_dist` and `dftracer_replay`.
  The option wins over the variable, and the default is `auto`, which is MPI under a launcher and one local rank otherwise.
  The same binary runs under any MPI or PMIx launcher, and a run with no launcher is one rank with the same code path.
- Share each node's cores between its ranks, by the CPUs each rank may use on Linux and by the rank count on macOS, so ranks that a launcher bound to their own CPUs keep them.
  One rank on a node keeps its `--executor-threads` and `--io-threads` values.
- Add `examples/comm/` (`comm_basics`, `comm_fault`, `comm_rank_report`), run by ctest as `examples/comm/*`, and the guide "Coordinate ranks with a Communicator".

- `dftu.svc.ports` has channels, latest values and phase results, with the C++ wrappers `Host::ports()`, `Ports` and `Channel<T>` in `dftracer/utils/plugins/plugin/ports.h`. A channel is declared in the plugin factory with `channel_open` and sent to and received from by tasks; it has credits, a message size limit and an overflow mode (`DFTU_OVERFLOW_BLOCK`, `FAIL` or `DROP_OLDEST`), and a receive into a short buffer gives `DFTU_PORT_TOO_BIG` with the needed size and keeps the message. A latest port is a hint with a host-assigned 64-bit generation and `DFTU_PORT_NOT_READY` before the first set. A phase result merges the contributions of many slices by `SUM`, `MIN`, `MAX`, `UNION` or `CONCAT` and is read for a sealed generation. `try_publish` and `try_consume` give the status of a batch port. `co_await ports.recv(id, buffer)` gives a `Result<uint32_t>`, and its errors carry a `PortError` code. The host prefixes a plugin's names with `plugin.<name>.`, and refuses the plan, naming the cause, for a duplicate namespace, a channel no plugin declares, ends that disagree, a skipped generation, a cycle through channels or `provides` and `consumes` (the message lists the plugins and names on it), and a `DFTU_OVERFLOW_BLOCK` channel whose ends share the fused scan stage or lack a `DFTU_CHANNEL_F_SENDS` or `DFTU_CHANNEL_F_RECEIVES` bit. Only a Block channel adds an order edge, from each sender to each receiver, so a request channel and a response channel are no cycle. `Channel<T>::recv` uses the new `channel_recv_exact` and drops a message whose size is not `sizeof(T)`. The host closes the channels of a scan that `Plugins::attach` joins to a session when the scan ends, and refuses at run time a Block `channel_send` started from `on_batch` or `transform`. A message send or receive that needs no wait makes no stop state and no scope, and latest and phase calls lock one port, not the hub.
- Add phase seeds to `dftu.svc.ports`: `phase_seed` (C++ `Ports::seed`, `PluginBuilder::ports()`) makes a phase result generation hold the earliest event start, the latest event end or the event count of the input files, filled and sealed by the host before the scan, so a plugin reads it in its first batch. The host takes the values from the index statistics when every file has a current index, else from a cache in the index root keyed by the files' path, mtime, size and sample hash, else from a pass of its own over the files, and never from a stale source. A seed with another merge or value type, an undeclared or unseeded-predecessor generation is a plan error that names the phase, and a plugin cannot contribute to a seeded generation. `PluginRun::seed_source` tells which source filled the seeds. The `two_phase_normalize` example reports event times from the start of the whole input in one scan, and the plugin guide says when to use a seed, a sketch or two runs.
- `dftu_svc_task` has `test`, which tells without waiting whether a task finished, and `cancel`, which returns at once and lets the task complete once, as `DFTU_OK`, `DFTU_E_CANCELLED` or another error, and `retain` and `release`, which let a thread that holds a reference hand a task to another thread that may `test` and `cancel` it; the task stays valid until the last reference is released.

- `dstd::fmt_append_join<", ">(out, range)` appends the items of a range with a separator between them, each as a plain `{}` hole, for lists whose length is known only at run time.

- `dftu.svc.store`, a plugin service over the store of the index root the scan reads, lent in namespaces the host names `plugin.<plugin name>.<ns>` so a plugin reaches only its own data and a name that would escape it is refused with `DFTU_E_INVALID`. It forwards the store C ABI function for function (namespace, batch, put, fold, erase, erase range, commit, snapshot, get, multi get, prefix and range scans, cursor next/key/value and the matching free calls) with an error on the callback's `ctx`, has no open, close, pack, drop, flush, publish, fail or refresh because the host owns the store, and is NULL on a cursor or build host, which run over no trace folder. A C++ wrapper is `Host::store()` in `dftracer/utils/plugins/plugin/store.h`. A commit only commits: the host flushes its one writer of the directory from the fold's finalize, whichever fold of the plugin committed, and a failed flush fails the run instead of only being logged. A namespace, batch or snapshot handle belongs to the store directory rather than to the fold that made it, so a handle a slice step made works in the master finalize and one of another root is refused; a NULL handle is refused with `DFTU_E_INVALID` and a message, and a refused commit still frees its batch. A scan over two index roots makes every call fail with `DFTU_E_INVALID` and a message that names both roots, so one store never serves two roots.

- The plugin ABI shape is recorded, not only its version: `cmake/plugin_abi_manifest.txt` holds one line per struct size and alignment, field offset and size, enumerator value, integer and string `DFTU_*` constant, typedef underlying type and function pointer spelling, with parameter names stripped so a rename changes nothing. `scripts/plugin_abi_manifest.py` writes it from the plugin ABI headers, ctest `plugin_abi_manifest` fails on any difference, `plugin_abi_manifest_update` records a shape change, ctest `plugin_abi_manifest_self` checks the parser on a fixture header, and `scripts/check_plugin_abi_manifest_diff.py` compares it against the newest release tag of the form `v<major>.<minor>.<patch>`: an added line, and a struct that grew with its alignment and its old fields unchanged, pass; a removed or changed line fails unless the `abi` major in `VERSION` moved.

- `DFTU_PLUGIN(name, version, ...)` in `dftracer/utils/plugins/plugin/register.h` writes a C++ plugin's whole entry point. The body is a lambda or a function name that takes a `PluginBuilder&` and a `const Config&` and returns `dstd::Result<void>`; the macro writes the info record and the `dftracer_plugin` symbol, calls it, and turns a returned error into the failed load with that error's condition and message. The last argument is variadic, so a comma in the body outside parentheses, as in a structured binding or a braced initializer, does not end the macro's arguments. A `PluginBuilder` destroys a fold it still holds when it is destroyed, so a body that registers one and then returns an error, and a caller that drops the builder unbuilt, leak nothing; `build()` hands the descriptor to the host and keeps nothing to destroy. The CLI scaffold, the example plugins and the test fixtures are written with it.
- `make_error(dstd::Condition, message)` with one helper per condition, `invalid_argument`, `io_error`, `unsupported`, `internal`, `not_found`, `parse_error`, `cancelled` and `out_of_memory`, re-exported by the SDK's `plugins::` namespace, so a plugin reports an error by condition instead of a C status image and never spells a `dstd::Condition` by hand, plus `status_of` and `report`, which carry an outcome to a callback's status.
- The C++ plugin SDK has a zero-copy frame builder in `dftracer/utils/plugins/plugin/frame.h`. `Column` takes a plugin's buffers by move (`from_int64`, `from_int32`, `from_uint64`, `from_double`, `from_bool`, `from_timestamp`, `from_duration`, `from_strings` from int32 or int64 offsets and bytes, `from_static_*` for memory the plugin owns for the process) and hands them to the engine with `dftu_series_new_flat_borrowed` / `dftu_series_new_string_borrowed`, so no column data is copied; `FrameBuilder::add` and `build()` name the columns and make the frame, a missing row, a null column or a row count that differs from the first is an error, and a failed build frees every column it made. The release callback runs once, when the last column that uses a buffer is gone.
- One frame owner type for the plugin surface: `OwnedDataFrame` in `dftracer/utils/plugins/plugin/frame.h`, with `get()`, `release()`, `reset()`, `is_null()` and `operator bool`. It replaces both the frame handle of `plugins/result_registry.h` and its own duplicate in the SDK, so a source, a node, a format, a UDF result and a named result all hand back one type. The scaffold templates and the example plugins use the builder in place of raw `dftu_series_new_*`, `dftu_series_free` and `dftu_dataframe_new` calls.
- The plugin SDK's frame and read types live in the dataframe library: `dataframe::Column`, `dataframe::OwnedDataFrame` and `dataframe::FrameBuilder` in `dftracer/utils/dataframe/plugin_frame.h`, still spelled `Column`, `OwnedDataFrame` and `FrameBuilder` in `plugins::`, plus two borrowed views built in place with no allocation. `ColumnView` (`type`, `size`, `is_null`, `values<T>` over a flat fixed-width column, `string_at` at any encoding, and `flat()` for a column that is not flat, which shares the buffers of a FLAT column and materializes any other) and `FrameView` (`column_count`, `row_count`, `name(i)`, `column(i)`, `column(name)`). A UDF reads a batch through `UdfBatch::arg(i)`, a `ColumnView`, gives `dstd::Result<Column>` from `exec`, and gets `dataframe::TypeId` from `UdfBind::type` and `bind.result`, with a `ScalarView` from `UdfBind::constant` and an `UdfFlag` set in `UdfOptions`; a UDAF gives `dstd::Result<Column>` from `finalize` and sets an `UdafFlag` set. So a C++ plugin writes no `dftu_*` type, call or `DFTU_*` constant outside its entry point. `dftu_dataframe_column_at` lends a frame's own column handle with no allocation.
- Golden test output prints a double in the shortest form that reads back as the same value (`0.1234567`, `55.74255561017869`), in place of six fixed decimals (`91.500000`), so a recorded golden keeps every digit.
  A whole value prints as an integer, exactly as the engine's JSON output does; every other value takes the shortest round trip, where the JSON printer uses `dstd::fixed_text`.
- `dftracer_plugin new <kind> <name> [--dir DIR]` writes one standalone CMake project for a `fold`, `udf`, `aggregate`, `format`, `sink` or `node` plugin: `CMakeLists.txt`, `plugin.cpp`, a `README.md` and a golden test that passes on the generated code. `dftracer_plugin check <lib> [--tests DIR] [--record]` loads a built plugin in a child process with the memory debug mode, the exit check and `DFTRACER_UTILS_PLUGIN_CHECK=1` on, prints its name, version, ABI version and every kind list, runs the golden tests of DIR, and reports a load error, each test result with a diff, the child's leaks or the signal that killed it; it exits 0 only when all pass. `--record` rewrites each expected block with what the query gave.
- Golden tests are plain `*.test` files in the style of sqllogictest: an `input` path relative to the file, a duql `query` block (with `rowsort`, or `error <text>`), `----`, then the expected rows, tab separated, with the column names first. A blank line ends the block, and one file holds several blocks.
- A plugin may register a file format or a sink under its own bare name, such as `toy` from plugin `toy`, as well as `<plugin>.<name>`. Any other name, another plugin's name and `dftu.` are still refused at load.
- Add `examples/plugins/csv_reader.cpp`, `examples/plugins/sessionize.cpp` and `examples/plugins/range_provider.cpp`, one example for the format, node and provider kinds, so every kind the SDK offers has one.
  `csv_reader` reads a CSV file and types each column and hands each chunk's rows straight out of the shared table, `sessionize.gaps` appends a session number per pid, and `range_provider.numbers` builds its table in code.
  Each returns a projection as exactly the columns it names, in its order, so a repeated name gives the column twice, and `sessionize.gaps` reads `pid` and `ts` through a materialized flat view, accepts a TIMESTAMP or a DURATION, and gives a row with a null pid or ts the session -1 without touching the walk.
- Run each example plugin that loads on its own through `dftracer_plugin check` in the default `tests` preset, one ctest each under `ctest -R examples/plugins/`, with the golden tests in `examples/plugins/tests/`.
  Each example must load, pass the golden tests it ships and leave no leaks, so CI builds and runs one plugin of every kind.

- `Plugins::describe()` reports the names each loaded plugin registered, one list per kind (`ops`, `udfs`, `udafs`, `states`, `providers`, `nodes`, `index_extensions`, `file_formats`, `sinks`) plus `has_fold`, and `dftracer_run --describe` prints them. Python: `Plugins.describe()` returns one `PluginInfo` named tuple per plugin, and `dft.scan_file(path)` plans a read through the plugin format that claims the file.
- Set `DFTRACER_UTILS_PLUGIN_CHECK=1` to make the plugin host validate every frame and column a plugin returns (provider, file format and node cursors, a UDF's `exec`, a UDAF's `finalize`, a plugin op's result and a fold transform's output), failing the run with an error that names the plugin, the callback and the broken rule. Off by default.

- A plugin can register a file format (`DFTU_SVC_FILE_FORMATS`, `plugins/abi/file_formats.h`): it scores a file 0..100 from its first 4 KiB, opens it, declares its columns, and hands the engine one cursor per chunk. `LazyFrame::scan_file` / `dftu_lazyframe_scan_file` scan a file through the highest-scoring format, `PluginBuilder::file_format<F>()` is the C++ SDK for one.

- A plugin can register an output sink (`DFTU_SVC_FILE_FORMATS`, `dftu_sink_vt`, `PluginBuilder::sink<S>()`), and every surface writes through it: `LazyFrame::sink(name, options, batch_size)` returns a `SinkSummary` of the rows and batches it wrote, `dftu_lazyframe_sink` is the C ABI, `LazyFrame.sink` in Python takes the options as keywords, `dftracer_view --sink NAME --sink-option KEY=VALUE` sends the query to it, and a duql pipeline ends with `sink "name" key "value"`, which returns one row of `rows` and `batches`. A bind, scan or write error aborts the sink once before it returns.
- A duql `call` stage can name a registered plan node: `select name, dur | call ns.n()` runs the node table to table with no arguments.
  The node reads the declared columns the plan carries at that point, so after a `select` it reads the columns that `select` names, and its output columns are the schema it declares for that input.
  A plugin node on a plan with undeclared columns now runs with those columns dropped, where it was refused before; name a field in a `select` to pass it.
  A name that both a plugin op and a node carry runs the op.

- ``dftracer/utils/arrow/stream.h`` streams a ``dataframe::Series``, a ``dataframe::DataFrame`` and a ``dataframe::LazyFrame`` as an ``ArrowArrayStream`` and imports one back as a chunked column, a frame, or a lazy frame that pulls one batch per scan; Python gains ``Series.__arrow_c_stream__``, a per-chunk ``DataFrame.__arrow_c_stream__`` and ``LazyFrame.__arrow_c_stream__``, and the schema reconciler moved into the arrow library.
- The arrow library has one owner type, `arrow::Batch`, in `dftracer/utils/arrow/batch.h`, and `dftracer/utils/arrow/ipc.h` has `arrow::Writer`, `arrow::Reader`, `arrow::scan_ipc` and `arrow::read_ipc`; `to_arrow`, `series_from_arrow` and `dataframe_from_arrow` report failure as `dstd::Result`, and `arrow::WriterOptions` picks the IPC compression.
  `arrow::scan_ipc` decodes several files at once, as many as the request's memory budget holds, gives their batches in file order, and rejects a file whose columns differ from the first file's.
- Add user-defined aggregates for plugins: `dftu_svc_ops::register_udaf` takes a `dftu_udaf_desc` (`dataframe/c_api_udaf.h`) with `bind`, `init`, `update`, `combine`, `finalize`, `destroy` and a `serialize` pair, and the host owns the memory of every group state.
  A state that is plain bytes (`DFTU_AGG_STATE_POD`) needs no `destroy` or `serialize`, and any other state needs all three, or the load fails with a message.
  `update` runs on whole columns and gets a null state pointer for a row with a null argument unless `DFTU_AGG_NULL_AWARE` is set.
  The SDK has `PluginBuilder::udaf<A>` with `UdafOptions` and `UdafStateSet`, and `dataframe::make_udaf<A>()` registers the same class in the same process and returns an owning `Udaf`.
- Run a registered UDAF as a window function: a duql `window` block calls it with `over N rows` or `over <width>` (slid with `retract` and `value` when the UDAF has `DFTU_AGG_HAS_RETRACT`, in slices of at least 1024 rows that step together so each call takes many rows, else merged from a segment tree with `combine`, O(log w) merges per row of a frame of `w` rows) or over the whole partition, and the C ABI has `DFTU_WINDOW_FRAME_UDAF` with `udaf`, `args` and `arg_count` in the frame of `dftu_window_spec`.
- Run a registered UDAF in `DataFrame::group_by`, `LazyFrame::group_by` and the C ABI group-by calls through `GroupAgg::fn` and `Agg::Udaf`, with its states spilled by `serialize` and fed from one thread unless it is `DFTU_AGG_PARALLEL_SAFE`; duql calls one by name in a `group` or `agg` block.
- Call a registered UDAF from Python as `dft.udaf["name"](dft.col("x"))` inside `DataFrame.group_by`, `LazyFrame.group_by` and `group_by_dynamic`; `dft.udaf` is a read-only mapping of the registered names, like `dft.udf`.
- Add scalar UDFs for plugins: `dftu_svc_ops::register_udf` takes a `dftu_udf_desc` (`dataframe/c_api_udf.h`) with `bind`, `exec` and `free_bound`, and duql calls the UDF by its name in `derive`, `select`, `where` and join keys.
  `exec` runs on whole columns, the host nulls the rows with a null argument unless `DFTU_FN_NULL_AWARE` is set, and a name that a built-in function or another UDF holds fails the load.
  A long `exec` polls `dftu_check`, so a cancelled or timed out query stops within one batch, and a plan that holds a UDF keeps its plugin library loaded.
  The SDK has `PluginBuilder::udf<U>` with `UdfBind` and `UdfBatch`, and `examples/plugins/duration_udfs.cpp` registers two UDFs.
  A UDF is one registration for every surface: Python calls it as `dft.udf["name"](...)` or `dft.call("name", ...)`, C++ as `dataframe::udf("name")`, and `dftu_op_find` lists it.
  `dataframe::make_udf<T>()` registers the same class in the same process and returns an owning `Udf`, which removes the name when it is destroyed while plans that hold it keep running.
- Add the plugin service `dftu.svc.memory` (`abi/memory.h`), the `dftu_svc_memory` table.
  Every loaded plugin has one memory pool named after it, so its blocks and reservations show in the pool gauges.
  A slice reserves bytes with `reserve` and `try_grow`, which returns `DFTU_E_NO_MEMORY` with a message past the budget and leaves the reservation as it was.
  A reclaimer set with `set_reclaimer` runs when a grow needs room, and the host releases and logs a reservation that a plugin leaves at unload.
  The SDK has `Host::memory()`, with `Memory` and an RAII `Reservation`.
- Add the plugin services `dftu.svc.json` and `dftu.svc.yaml` (`abi/format.h`), one `dftu_svc_format` table served under both ids.
  A plugin parses a text into a `dftu_value` tree, gets `DFTU_E_PARSE` with a `line:column: reason` message for a bad text, and writes a document with a streaming writer that returns `DFTU_E_INVALID` for a misuse.
  The SDK has `Host::json()` and `Host::yaml()`, with `Doc` and `FormatWriter`.
  The new `json::Writer` writes compact JSON the way `yaml::Writer` writes YAML.
- Add `Indexer.build_async()` and `LazyFrame.collect_async()`, which start the work on the native pool and return a `TaskHandle` at once.
  `get()` releases the GIL while it waits and raises the error that `build()` or `collect()` raises.
  A handle can be awaited from `asyncio` or `uvloop`, takes `add_done_callback` and `cancel`, and a cancelled await asks the native task to stop.
  The native `TaskHandle` is now a typed completion handle in place of the empty shell, and `dstd::Any` is no longer included by `trace/views/view.h`.
- Add `dstd::Condition::OutOfMemory` and the C status `DFTU_E_NO_MEMORY` (-12) for a refused allocation.
  The error code `MemoryBudgetExceeded` now has the condition `OutOfMemory` in place of `Internal`.
  The Python bindings raise `MemoryError` for an `OutOfMemory` error, such as a plugin callback that returns `DFTU_E_NO_MEMORY`.
  The plugin examples and the `dftracer_plugin` and JIT templates report `DFTU_E_NO_MEMORY` when a `malloc` fails.
- Add `dstd::try_reserve` for `dstd::Vector` and `dstd::String`, which returns `OutOfMemory` in place of ending the process when the heap refuses.
  The plugin file system service `stat_batch` uses it, so a count the heap cannot hold returns `DFTU_E_NO_MEMORY` with a message.
- Add the library `libdftracer_utils_plugins`, with its pkg-config file and CMake package, between trace and domains.
  It holds the plugin host, which moves from `dftracer::utils::domains::plugins` to `dftracer::utils::plugins::host`.
- Add `Series.name`, kept through `from_pandas`, `to_pandas`, Polars, ops, `df["x"]` and pickle, with `dftu_series_name` and `dftu_series_rename` in the C ABI.
  A binary op keeps the name only when both names are equal, as pandas does, and `Series.rename` and `Series(data, name=...)` set it.
- Add `yaml::Records` for a YAML text too large for memory.
  It yields the elements of a top-level block sequence, or the documents of a `---` stream, one at a time, from a string or from blocks that the caller reads and gives it.
  The yaml library reads no files.
  Each chunk of about 64 MiB (`RecordsOptions::chunk_bytes`) is parsed on its own and its buffers are reused, so memory follows the chunk and not the text.
  `Records::next_chunks` gives a batch of `RecordsOptions::batch_chunks` chunks, which the caller can parse and read on its own threads.
- Add `yaml::Document::parse` with a `yaml::RunJobs` hook that indexes one large text in chunks on the caller's threads and joins them, with the same result and errors as the one-thread parse.
  A 200 MB text parsed 5 to 7 times faster on 14 threads and 7 to 9 times faster on 48.
- Add the Arrow buffer getters `dftu_series_validity`, `dftu_series_chunk_count`/`dftu_series_chunk`, `dftu_series_dictionary_codes`/`dftu_series_dictionary_values` and `dftu_series_view_buffer_count`/`dftu_series_view_data`/`dftu_series_view_buffer`, add `dftu_series_new_string_borrowed`, and make `dftu_series_new_flat_borrowed` and `dftu_series_new_string_view` borrow the validity bitmap, so a plugin builds Arrow arrays from a column and hands Arrow memory back with no copy (`examples/plugins/arrow_convert.c`).
- Add a dataclass config for `@jit.plugin`: annotate the class with `config: Cfg`, where `Cfg` is a (frozen) dataclass of `int`, `float`, `bool` and `str` fields or `Optional` of one, and read `self.config.<field>` in the body.
  Defaults come from the dataclass, `Plugins` checks the values before the load and raises `JitError` that names a missing required field, an unknown key or a wrong type, and a field of another type is a compile error at the field.
  Each loaded plugin instance holds its own config, so two instances of one plugin with different values do not disturb each other.
  A `str` field is copied into the instance, compares with an event string and may be a key, and an `Optional` field set to `None` stays unset even when its default is a value.
- Name the position of every JIT compile error, `JitOpError` included (it is now a `JitError`): a `JitError` reads `<file>:<line>:<col>: <reason>` and then the source line, and carries `file`, `line`, `col`, `source` and `reason`.
- Check the public `jit` surface with `mypy --strict` as well as `ty`, from the fixtures in `tests/python/typing/`, and type `jit.each_event`, `jit.record`, `jit.vfold`, `jit.series` and `JitPackage.plugin` and `JitPackage.series` so a wrong use is a type error.
- Add plain-container scan state for `@jit.plugin`: annotated class fields `Dict`, `DefaultDict`, `Set`, `List` (with `Tuple` keys) and `int` or `float` scalars lower to the existing accumulators, and the merge across worker slices is inferred from the update code only: `d[k] += x` and `d[k] = d.get(k, 0) + x` sum, `d[k] = max(d[k], x)` and `max(d.get(k, x), x)` (and `min`) take the extremum, `s.add(x)` unions, `lst.append(x)` concatenates in event timestamp order and `n += x` sums a scalar.
  Any other update, a read of the field in the body, a field the body never updates and a non-empty start is a compile error at its line, and a missing key follows Python: `d[k] += x` on a plain `dict` is a compile error unless the field is a `defaultdict`.
  `jit.map` descriptors stay for `mean`, `variance`, `quantiles`, `topk`, `argmax`, `distinct`, product values and `order_by=`.
- Add `jit.Store[K, V]`, a persistent key-value field lowered to the plugin store ABI: `self.runs.get(key, default)` and `self.runs.put(key, value)` with an `int`, `str` or `Tuple` key and an `int` or `float` value, written in the encoding of `store.Namespace[K, V]`.
- Add conditions to an `@jit.each_event` body: `if` / `elif` / `else`, nested ifs, several guards, `and` / `or` / `not`, chained comparisons, `in` a literal tuple, list or set, and `is None` / `is not None` on an optional config field or a string field. A body that is one `if` around its updates lowers the guard to a mask over the batch and then updates only the matching events; the rest branch per event. `plugin._jit_plugin.lowering` says which. A walrus, a call outside the primitives and a comparison between a string and a number are compile errors with the line and column.
- Add `@jit.each_batch` on `@jit.plugin`: a body of column operations (arithmetic, comparisons, `&` `|` `~` masks, `s[mask]`, `where`, `abs`, `clip`, `isnull`, `notnull`, `fill_null`) that folds `sum`, `min`, `max`, `count` and `mean` into scalar fields or key-less `jit.map` descriptors. It lowers to fused engine expressions with no per-event loop; a column stored on `self`, returned, folded without a reduction or used outside the vocabulary is a compile error. Add the typed `jit.Frame`, `jit.Column` and `jit.Mask` for the body and `benchmarks/jit_batch_bench.py`.
  A reduction folds only into the target that merges it exactly (`sum` and `count` into `+=` fields and `jit.sum()`/`jit.count()`, `min` into `jit.min()`, `max` into `jit.max()`, `mean` into `jit.mean()`, which observes each element); any other pair is a compile error.
- Make `/` in an `@jit.each_event` body true division on doubles (it was C integer division on integer fields while typed as a float); `//` stays a compile error.
  The namespace is the plugin name plus the field name, and a field annotated with a `Series`, `DataFrame` or `Expr`, or a `Store` of one, is a compile error.
- Add exceptions to a compiled `@jit.plugin` body: `raise jit.Fatal("msg")` fails the run, `raise jit.SkipRest()` stops the rest of the batch (or of a task) and keeps the work done, and `raise jit.Cancelled()` ends the run as cancelled. Each lowers to a status of the plugin ABI with the message copied, and `Plugins.run` raises it as `jit.Fatal` or `jit.Cancelled` (a `BaseException`) with `message`, `file`, `line` and `col` of the `raise`. `try` / `except jit.Error` (or `jit.NotFound`, `jit.IoError`, `jit.ParseError`) over a store get, a file or a JSON parse lowers to a status check; a bare `except`, an `except` of a Python class, `except ... as name`, `else` and `finally` are compile errors.
- Add `async def` to a compiled plugin: `@jit.task` marks a method with `int` and `float` parameters that lowers to a `dftu_step_fn` state machine, and `@jit.finalize` marks the method that runs when the scan ends (`def`, or `async def` that awaits). The awaits are `jit.sleep(s)`, `jit.all(...)`, `jit.any(...)`, a task call, a store get and a channel send or receive; the others are compile errors. A task cancelled at its `await` never runs on, and each task completes once. `jit.emit(name, value)` emits a named result as bytes.
- Add `with jit.open(path) as f` to a compiled body, with `for line in f` (text lines, gzip inflated) and `f.read(n)` (raw bytes), lowered to the file service. The path is a string literal or a `str` config field, and the file closes on every way out of the block, a `raise` included. A missing file raises `jit.NotFound` with the path. Plain locals (`int`, `float`, `str`) and `+=` on them are part of the body now.
  A line, a chunk and `v.as_str()` are views of bytes that the host lends: `==`, `!=`, `len` and `jit.emit` read the bytes, and the string is interned only as a key or an element of state. A view that outlives its loop, its next read or its document is a compile error.
- Add channels, latest values and phase results to a compiled `@jit.plugin`, declared as class attributes with the element type (`int` or `float`) in the annotation: `out: jit.Sender[int] = jit.sender(credits=, policy=)`, `inp: jit.Receiver[int] = jit.receiver(Producer.out)`, `q: jit.Channel[int] = jit.channel(...)`, `cfg: jit.Latest[int] = jit.latest()` and `total: jit.Phase[int] = jit.phase(merge="sum")` (`"min"` and `"max"` too). `send` and `try_send` work in `each_event`, an `each_batch` reduction, `finalize` and tasks, `await self.q.send(v)` waits for a credit on a `"block"` `jit.channel`, `v = await self.inp.recv()` raises the new `jit.Closed` at the end of a closed channel and `async for v in self.inp:` ends there, `close()`, `latest.set` and `get(default)`, `phase.contribute`, `seal` and `result(default)` lower to `DFTU_SVC_PORTS`. The plugin factory declares each end, the plugin name is its identity, a sender's plugin is ordered before its receivers' and every receiver lists its channel in `consumes`, a string receiver of a `jit` sender is checked for its element type, a plan error of the host, such as a receiver with no sender, reaches `Plugins(...)` as a `JitError` located at the port. A misuse (a send on a receiver, `recv` in the per-event body, a Block channel in the scan or between two plugins, a `str` port, a float into an `int` port) is a compile error with the location.
- Make a compiled body safe at the edges: a map that a `for` fills grows its row buffers at every emit site and fails with `DFTU_E_NO_MEMORY` before 2^31 rows, a failed or repeated `jit.json` / `jit.yaml` parse leaves the local on a live document, a task cannot get or put to a store, and a runtime error carries its `jit.<Kind> [file:line:col]` tag first so that a long path cannot cut it.
- Add `jit.json(text)` and `jit.yaml(text)` to a compiled body: a typed value with `find`, `as_int`, `as_float`, `as_str`, `as_bool`, `is_null`, `len`, iteration over array items and `items()` over object members. A missing key, a null and a value of the other kind give the default; a parse error raises `jit.ParseError` with the line and column.

### Changed

- Write the original event lines in `TraceViewer.sink_trace` when the viewer holds only its own events (no column op, select, row window or `partition_by`), so a top-level or nested key that no column carries stays in the output.
- Write C++ `View::sink_trace`, `dftracer_view --merge` and row materialize through the same output writer as the Python sinks: the output is staged beside the target and renamed over it, a failed write leaves the target as it was, and the index is built while the files are written in about half the memory.
  `TraceWriteOptions::part_size` now makes `output_path` a folder of `part-NNNNN.pfw.gz` files, `worker_count` 0 uses every core, and a missing parent folder is created.
- Keep the view's `select`, offset and limit in `View::sink_trace`; they were dropped.

- Remove `write_table`, `read_table` and `TableWriter` from `dftracer.utils`, `TraceViewer.export_trace` and `TraceViewer.sink_json`, and the `LazyFrame.sink(name, **options)` form. Write with `DataFrame.write` and `sink`, read with `TraceViewer(path).collect()`. A trace viewer's scan counts are read through the private `_collect_counted`.
- Regroup the Python API so each public name has one import path, and make every module that holds no public names private. `dftracer.utils` keeps the DataFrame engine and the table functions (`DataFrame`, `Series`, `LazyFrame`, `GroupBy`, `Expr`, `F`, `col`, `lit`, `call`, `where`, `when`, `if_else`, `eval_many`, `count`, `Agg`, `AggOp`, `DType`, `TimeUnit`, `GroupKey`, `Udf`, `udf`, `Udaf`, `udaf`, `lazy`, `collect_all`, `scan_file`, `LazyResult`, `LazyScalar`, `BoundColumn`, `SinkSummary`, `read_table`, `write_table`, `TableWriter`). The rest moved: `dftracer.utils.trace` has `TraceViewer`, `LiveTraceViewer`, `LiveFileStatus`, `Containment`, `IndexRecords`, `TraceWriter`, `Phase` and `discover_trace_files`; `dftracer.utils.index` has `Indexer`, `CheckpointIndexer`, `Index`, `IndexStatus`, `AggregationConfig`, `BloomConfig`, `hash_value`, `write_shard_set` and `consolidate_shard_set`; `dftracer.utils.runtime` has `Runtime`, `TaskHandle`, `get_default_runtime`, `peek_default_runtime`, `WorkerShare`, `worker_share`, `apply_worker_share`, `MemoryBudgetAdvice`, `memory_budget_advice` and the log functions; `dftracer.utils.errors` has the `DFTUtils*Error` classes; `dftracer.utils.store` has `Store`; `dftracer.utils.plugins` has `load_plugin` and `PluginHandle`; the new `dftracer.utils.typing` has `SeriesGroupBy`, `LazyGroupBy`, `Rolling`, `Ewm`, `Named` and `Value`; `dftracer.utils.jit` also has `JitOpError`, `can_fuse`, `lower_to_expr` and `run_op_array`. `Phase` left `enums`, `where` and `eval_many` live in the expression module, and the modules `agg`, `dataframe`, `enums`, `expr`, `groupby`, `lazyframe`, `series`, `table`, `jit_op` and the implementation modules of `trace`, `store`, `dask`, `duql` and `jit` are now private (leading underscore). `dftracer.utils.pandas` and `dftracer.utils.polars` stay as compatibility namespaces that re-export the engine names. The old top-level names and module paths are gone.
- The comm observe trace writes `ph` as the integer `1`, as DFTracer does.
- Keep the integer type in a group `cumprod` and in the `running_prod` window: an int64 column gives int64, a uint64 column gives uint64, and a product wraps on overflow as pandas does. A float column stays float64.
- Rename `collect_typed` and `typed` to `collect_index_records` and `index_records` (C++ `View`, Python `TraceViewer`, `DaskTraceViewer`), which now return an `IndexRecords` with `regular`, `aggregated` and `counters` fields in place of a dict; the `dftracer_view` flag `--collect-typed` is now `--index-records`.
- Speed up the dataframe ops that were slow against pandas 3.
  `DataFrame::column_index` looks a column up through a hash of the names that the first call on a frame of 16 or more columns builds (a frame grows by 8 bytes for it); a Python `DataFrame[name]` on a frame of 100k columns no longer scans the names.
  `unstack` numbers the rows and columns with the native hash numbering and takes each value column once at the row that fills each cell, in place of four sorts and a pivot per value column; it reads NaN labels as one label, as pandas does.
  Sorting a column with nulls (`sort_values`, `sort`, `argsort`, `top_k`, `bottom_k`) runs the SIMD sort on the rows that hold a value and puts the nulls first or last in input order, where it ran a comparison sort that read each row through the C ABI; `unique`, `drop_duplicates` and `value_counts` number 1 to 8 byte values (floats by their canonical bits) on an open-addressing table, in runs on several threads when the first rows repeat; `value_counts` orders the counts with a counting sort and the pandas-named one uses it.
  Label lookups on whole-number labels run on several threads (`reindex`, `loc` with a list of labels), `reindex(method="ffill" | "bfill")` on monotonic whole-number labels searches from the previous answer, and `align` of two increasing Int64 label sets walks both once.
  Zoned `resample` with a day or week rule builds its edges by arithmetic and converts the column to wall time once.
- Key a float group-by, `unique` and `nunique` value, `value_counts` value, join and merge key, and `isin` and `mode` value as pandas and Polars do: -0.0 is 0.0, and every NaN, whatever its sign and payload, is one value. The group-by (`DataFrame::group_by`, `group_agg`, the pandas `groupby`) and the native `value_counts` hashed and compared the raw bits, so -0.0 and 0.0 made two groups and NaNs of different payloads did too. The key column of a result shows the zero as 0.0. `isin` finds a NaN with a NaN needle, `mode` returns 0.0 for a zero of either sign and skips NaN, and a join matches -0.0 with 0.0 and a NaN with a NaN. The `lookup` and `nest` joins key a NaN too, where a NaN key matched nothing.
- Keep the length of the input in the pandas `groupby` transforms (`transform`, `cumcount`, `ngroup`, `rank`, `shift`, `cumsum`, `diff` and the rest) when a key is dropped: a row with a null or NaN key is null in the result, as pandas gives NaN. A NaN key is a null for `dropna=False` too, one missing group. An all-NaN float Series with integer `bins` in `value_counts` raises `ValueError`, as pandas. An integer result with such missing rows is float64, as in pandas; with every key missing the result is all NaN on every row. `rolling` and `expanding` leave the dropped-key rows out, as pandas, and `groupby(k)[v].expanding()` no longer crashes on a null key. `DataFrame.take` raises `IndexError` for an index past the last row, where it read outside the columns.
- Give the rows of the pandas `groupby` `apply`, `filter`, `get_group`, `head`, `tail` and `nth` the labels they had in the frame (the positions of the frame when it has no index), also when a null or NaN key left rows out. A traced `filter` with `dropna=False` keeps the group of the missing key, and `get_group((nan,))` of a single key raises `KeyError`, as pandas. `shift` and `diff` of an integer column, the rolling windows and the expanding windows give float64 as pandas does, and `shift` takes `fill_value` (an integer column stays integer; a fill the column cannot hold, such as text for a number column, raises `TypeError` where pandas gives an object column). The running product `cumprod` of an integer column stays float64, exact up to 2**53 (pandas keeps int64 and wraps). A negative index in `take` gives a null row; this is now in the docs.
- Return the pandas shape from `Series.value_counts` and `DataFrame.value_counts`: a Series of counts named `count` (`proportion` with `normalize=True`), labelled by the distinct values (one level per column for a frame), where each gave a frame with a `value` or key column and a `count` column.
  `Series.value_counts` takes `normalize`, `sort`, `ascending`, `bins` and `dropna`, and `DataFrame.value_counts` takes `subset`, `normalize`, `sort`, `ascending` and `dropna`, in the pandas order. Ties keep the order of first appearance, `sort=False` lists the values by first appearance, and `dropna=False` counts a null and a NaN as one missing value. `bins` counts the values per interval of `pandas.cut`, every interval listed with its count (zero too), labelled with the interval text such as `(0.999, 2.0]`; with no value in any interval it is empty, as pandas. A key column of a frame may have any name, `count` included.
  Among tied counts a null sits at its first appearance, where pandas puts the null of a nullable dtype last.
  The lazy group-by `value_counts` keeps its frame, `Expr.value_counts` keeps its struct, and the native `Series.value_counts` of the C++ and C surfaces still returns the frame; the Polars frame of `value` and `count` is no longer a `Series` method.
- Return a Series from the pandas `groupby(...).size()` when the keys are the index (`as_index=True`), unnamed and under the keys, as pandas does, so `df.groupby(["a", "b"]).size().unstack()` reads it; the Polars `group_by` and `as_index=False` keep the frame with the `size` column. `g["c"].sum()` and the other reductions of one column still return a one-column frame.
- Change the shape of the pandas `groupby` results that lead with the group keys: `SeriesGroupBy.nlargest` and `nsmallest` are a Series named like the column in place of a frame of keys, `index` and the value; `value_counts` is a Series named `count`; `take` gives the rows in group order without the key columns; `apply` gives frames and Series under the keys, and its group function gets the frame without the key columns, as pandas 3.
  A scalar or a list per group in `apply` is a Series with no name under the keys, `g[["a", "b"]].sum()` is a frame with those columns, and `value_counts(sort=False)` orders the combinations by count with ties in order of first appearance.
  Without `append`, `DataFrame.set_index` drops the old index columns, as pandas.
  The Polars `group_by` and the lazy group-by keep their shapes.
- Read a pandas frame whose `RangeIndex` does not start at 0 or has a step as row labels, where it was dropped; `DataFrame.from_arrow` does the same for the range in the pandas metadata.
- Cut the Python names that were public by accident, and give every public module an explicit `__all__` with exactly its public API. `tests/python/test_public_names.py` checks each public module: the list exists, each name resolves, none is a module, a type variable or an imported name, and each is in the docs or listed with a reason.
  `dftracer.utils.columnar` (the function) is gone: call `Expr.apply(source)` or `DataFrame.apply`; `eval_many` and `where` stay.
  `dftracer.utils.runtime`, `index`, `dataframe`, `lazyframe`, `series`, `enums` and `plugin_cli` had no `__all__`, so every import in them (`os`, `json`, `Callable`, `TypeVar`, `coerce_duration` and the like) looked public; each now lists only its API.
  `dftracer.utils.dask.__all__` adds `distributed_index`, `DistributedIndexResult`, `QueryPage`, `distributed_write_trace` and `DFTracerUtilsDaskWorkerPlugin`. `parse_bytes` and `parse_duration_seconds` of the enums module are now private (`_parse_bytes` and `_parse_duration_seconds`); `coerce_bytes` and `coerce_duration` do the same job for callers. The test also fails on a public class or function that a module defines and leaves out of its `__all__`.
  `dftracer.utils.jit.ops` is no longer in `dftracer.utils.jit.__all__` (import it as before). `dftracer.utils.jit.emit` the module is `dftracer.utils.jit._emit`, so `jit.emit` is always the function that `@jit.finalize` bodies call and never the module.
- Split the index into summaries and feeders. A summary (zonemap, bloom, counts, postings) knows only the algorithm: it takes typed values (int64, uint64, double, string, null) and writes its payload, and `Summary::compile` prepares a filter leaf against it.
  A feeder owns the format: the JSON feeder reads the events the scan already parsed, with no second parse and no frame per line, and the column feeder (`ColumnFeeder`) takes one frame per chunk with a path per column, so a reader that has no JSON line can feed the same summaries.
  The stored bloom, counts and postings bytes are the same as before.
- Make the zonemap richer. A zone keeps its bounds in the type of the values (int64, uint64, double, or a string compared as unsigned bytes), counts the null values and the NaNs, never makes a NaN a bound, and marks a bound that is not exact, such as an integer a double cannot hold: that bound never rules out a value and never proves that every value passes.
  A path that holds numbers and strings has no bounds. A filter literal is compared with the bounds as a number, so `x > 1.5` against an int zone is not read as `x > 1`.
  A zone starts with a payload version byte and the `index` version in `VERSION` is 2, so an index built before this change is built again; a payload of another version is not read and prunes nothing.
- The Python package passes `mypy --strict` and carries no `type: ignore` comment. `DataFrame.group_by` and `groupby` return a `GroupBy` without aggregates and a `DataFrame` with them; `collect_all` is typed by its roots; `Session` methods return `Handle[T]`; `Indexer.manifest`, `explain` and the status calls return typed dicts; `DataFrame.to_dict` is `dict[str, list[Scalar | None]]`; `polars.concat` and `pandas.concat` take `how` as a `Literal`. A test fails on `Any` in a public signature of `python/dftracer`.
- The index hashes each value by its type, once, for every bloom filter and posting list (`dstd/hash/value_hash.h`): every integer hashes as an int64, a double that is a whole number hashes as that integer (`42.0` is `42`, `-0.0` is `0`), any other double by its bits, every NaN as one value, a uint64 above the int64 maximum as itself and a string as its bytes.
  A query on `args.size == 42` now finds a column of `42.0`, and `42.5` never matches `42`; a number literal also probes the text of the number only on a path the catalog says may hold strings, or when the file has no catalog.
  Each stored bloom filter carries a hash version byte after its three header words, and the marker of each postings path stores it; a reader that finds another version does not prune and reads the chunk.
  The `index` version in `VERSION` moved for it, so an index built before this change is built again; a sharded view root made before it is refused with its format version and must be indexed again.
- Speed up the Python layer.
  `Series.compare`, `Series.equals`, `DataFrame.equals`, `first_valid_index`, `last_valid_index`, `GroupBy.groups`, `Series.repeat` and `Series.drop` run on the engine kernels in place of a Python loop over the rows.
  On 1M rows (median of 5 rounds): `Series.compare` 57 to 2 ms, `equals` 65 to 2 ms, the four valid-index calls 99 to 36 ms, `GroupBy.groups` 63 to 36 ms.
  `compare` and `equals` use the row compare kernel for numbers, bool, strings, binary, dates and times; a number against text differs on every present row (as pandas); nested, decimal and mixed time columns compare by value in Python.
  `compare` and `equals` match times by instant when only the unit or the zone differs (a naive timestamp never equals an aware one; a duration in seconds never equals one in milliseconds with the same integers, and 1500 ns never equals 1 us), and two NaN at the same position are equal, as in pandas.
  `GroupBy.groups` keeps the user column named `_row`, merges `0.0` and `-0.0`, returns `bool` labels for a bool key, and gives the positions of the original rows when null keys are left out.
  `import dftracer.utils` takes 31 ms in place of 47: the version comes from the generated `_version.py` (the build already writes it) and not from `importlib.metadata`, and `asyncio`, `concurrent.futures`, `inspect` and `logging` load on first use.
  Probes for Arrow and pyarrow in the pandas and polars converters run once.
  `benchmarks/python/micro_bench.py` times these calls.
- Move the Python modules to one table per fact. The deep import paths change; the names in `dftracer.utils.__all__` and their imports from `dftracer.utils` stay.
  - `dftracer.utils.columnar` is gone: the expression nodes, `Expr`, `Agg`, `Udf`, `when`, `F` and `ColumnOp` are in the expression module; `udaf`, `Udaf`, `count` and the aggregate helpers in the aggregate module; `GroupBy` in the group-by module; `Columnar`, `columnar`, `where` and `eval_many` in the evaluator module. All public ones are exported from `dftracer.utils`.
  - Helpers that other modules import lost their leading underscore (`Col`, `Lit`, `Bin`, `emit_ast`, `collect_columns`, `AGG_CODE`, `single_index`, `as_names` and the like).
  - `dftracer.utils.time_unit` and `dftracer.utils._units` are in the enums module (private, with `DType`, `TimeUnit`, `AggOp` and `GroupKey` exported from `dftracer.utils`), which now holds the time-unit and dtype tables (`UNIT_NS`, `UNIT_CODES`, `FREQ_NS`, `EPOCH_NS`, `CAST_DTYPES`) and the `DType` properties `is_integer`, `is_float`, `is_numeric` and `int_range`.
  - `dftracer.utils._import` and the conversion helpers of the `Series` module are in `dftracer.utils._convert`.
  - The `DFTUtils*Error` classes are in `dftracer.utils.errors`; `get_default_runtime` and `peek_default_runtime` are in `dftracer.utils.runtime`.
  - `dftracer.utils.indexing` is `dftracer.utils._loc`.
  - The `jit` internals are private modules of `dftracer.utils.jit` (types, monoids, prims, maps, compiler, emit, plugin, vfold and package); `dftracer.utils.jit` exports their names.
    The C scalar types of a jit body are one table keyed by `DType` (`C_TYPES` in the jit types module), and the Python operators that `apply`, `jit` and `jit.op` lower are one table in `dftracer.utils._eval` (`ARITH_OPS`, `COMPARE_OPS`).
  - `dftracer.utils._plugin_build` is `dftracer.utils._build`, the one compile pipeline of `@jit.plugin`, `@jit.op`, `@dft.udf` and `dftracer_plugin`; `PluginBuildError` is `BuildError`.
    It resolves the compiler, the flags and the ABI digest once per environment, so a cached `jit.op` call no longer re-hashes the ABI headers.
    A built plugin sits in `<cache>/<digest>/<name>.so`, a built op in `<cache>/op/<digest>/<name>.so` and a built UDF in `<cache>/udf/<digest>/udf.so`.
  - `dftracer.utils.dask` and `dftracer.utils.dfanalyzer` are packages that load a submodule on the first use of a name, so importing them no longer imports `dask`.
    `dftracer.utils.dask_frame` is a module of the `dftracer.utils.dask` package, as are the progress, worker, viewer and index code; the public names of each are exported from `dftracer.utils.dask`. The dfanalyzer helpers are split the same way.
    `_runtime_threads` is `runtime_threads` of the dask worker module.
  - `dftracer.utils.trace_viewer` and `dftracer.utils.live_trace_viewer` are in the package `dftracer.utils.trace`, which exports their public names; `dftracer.utils._trace_lazy` is a private module of that package and `_LazyOps` is `LazyOps`.
  - `dftracer.utils.indexer` is `dftracer.utils.index`, `dftracer.utils.duql` is a package (`col`, `pipe`) with the same names, and `dftracer.utils.store` and `dftracer.utils.store_codec` are the package `dftracer.utils.store` and its module `dftracer.utils.store.codec`.
- `TraceViewer(files)` and `Indexer(files)` take one input form: a `str` or `os.PathLike`, or a list or tuple of them. One directory is scanned; a directory inside a list is a `ValueError`; a path given twice is read once.
- Name the index location `index_dir` everywhere: `TraceViewer(files, index_dir=)`, `LiveTraceViewer(files, index_dir=)`, `CheckpointIndexer(gz_path, index_dir=)` and its `index_dir` property, `Index(index_dir)`, `write_shard_set(index_dir, ...)`, `consolidate_shard_set(index_dir)`, `distributed_index(index_dir=)`, `build_index_distributed(index_dir=)`, the dfanalyzer helpers (`index_dir_for`, `view_typed_frames`, `count_index_files`), `IndexStatus.index_dir`, the `index_dir` key of the Indexer status dict, of the manifest entries and of the `distributed_index` result, and the `index_dir` field of the `dftracer_stats` JSON.
  In C++ and C, the `index_dir` parameter of `View::from_directory`, `LiveView` and `dftu_view_from_directory`, and the `index_dir` field of `TraceWriteOptions`.
  The old spellings are gone.
- Change `Indexer` to `Indexer(files, ...)`, where `files` is a path, a folder or a list, as for `TraceViewer`, and read it with the same code.
  `ensure()` builds what is missing or stale and does nothing when the index is fresh, `build(force=False)` builds the missing tiers or, with `force=True`, rebuilds them all, and `status()` reads what needs work without building.
  Both return an `IndexStatus`.
  The `directory` and `force_rebuild` arguments and the `resolve` and `ensure_indexed` methods are gone.
- Export the error classes (`DFTUtilsError` and its ten subclasses) from `dftracer.utils.errors` and `DType` from `dftracer.utils`, and keep `schemas`, `Session` and `Handle` out of `dftracer.utils.__all__`.
  `Session` and `Handle` stay in `dftracer.utils.trace`, and the `typing`, `atexit` and `importlib` helpers no longer show as public names of the package.
- Make the polars names canonical on `Series`, `Expr`, `DataFrame`, `LazyFrame` and the group-by classes, and build every pandas spelling from one table, `python/dftracer/utils/_aliases.py`: `fillna`, `astype`, `isna`, `notna`, `dropna`, `isin`, `groupby`, `sort_values`, `drop_duplicates`, `take`, `cumsum`, `ffill` and the rest are the same object as `fill_null`, `cast`, `is_null`, `is_not_null`, `drop_nulls`, `is_in`, `group_by`, `sort`, `unique`, `gather`, `cum_sum` and `forward_fill`.
  A pandas name with other keywords or another result (`sort_values(ascending=, na_position=)`, `nlargest`, `groupby(sort=)`) is a small function listed in the table.
  `Series`, `DataFrame`, `LazyFrame` and `Expr` take the same keywords for a shared name: `fill_null(value)`, `clip(lower, upper)`, `is_between(lower, upper)` and `sort(by, *, descending, nulls_last)`.
  `sort` puts nulls first by default, as polars, on `Series`, `DataFrame`, `LazyFrame` and `Expr` (`Expr.sort(nulls_last=False)` runs, where it raised); `sort_values` keeps the pandas `na_position="last"` default.
  `Series.skew()` and `Series.kurt()`, `DataFrame.skew`, `DataFrame.kurt` and the group-by `skew` and `kurt` give the pandas unbiased values; `Series.skew(bias=True)` and `Series.kurtosis(fisher=True, bias=True)` give the polars ones.
- Make `.str`, `.dt` and `.list` agree between a `Series` and an `Expr`.
  A polars-only name (`dt.epoch`, `iso_year`, `ordinal_day`, `week`, `truncate`; `str.count_matches`, `strip_chars_start`, `strip_chars_end`, `strip_prefix`, `strip_suffix`, `to_titlecase`) is a method on both with polars results; a pandas-only name (`dt.dayofweek`, `is_month_start`, `floor`, `ceil`, `normalize`, `total_seconds`, `tz_localize`, `tz_convert`, `days_in_month`, ...) is a property on a `Series` and a method on an `Expr`.
  `Expr.str` methods take the keywords of `Series.str` (`pat`, `case`, `regex`, `n`, `errors`, `expand`, `width`, `sub`, `repl`, `group`, `start`, `stop`), so `col("s").str.replace("a", "b")` replaces every occurrence as `Series.str.replace` does, and `Expr.dt.round` takes `freq`.
- Make `Series.unique(maintain_order=False)` keep a null as one value (sorted, nulls first; first-occurrence order with `maintain_order=True`), as polars, and `Series.drop_duplicates(keep=)` give the pandas result; both dropped the nulls before.
  `arg_sort` takes `nulls_last` (default `False`, as `sort`) on `Series` and `Expr`, `search_sorted` takes a scalar and `side=`, and `Series.add`, `sub`, `mul` and `truediv` take a scalar; `divide`, `div`, `multiply`, `subtract`, `at`, `loc` and `iat` are table aliases.
  `DataFrame.kurtosis` gives the polars value per column and `kurt` the pandas one.
- Raise `DFTUtilsValueError` when a method meets a column of a type it does not take, on a `Series` and when an `Expr` runs, where a `Series` raised `TypeError` or `RuntimeError` and an `Expr` any of the three.
- Fix `Expr.str.to_titlecase`, which swapped the case instead of title-casing.
- Release the GIL around every `Series` kernel and every eager `DataFrame` op on 16384 or more rows, so two Python threads run two long kernels at once.
  `Series.to_pylist`, `np_parts` and the other calls that build Python objects keep the GIL.
- Make the native `Series` the public class: `dftracer.utils.Series` subclasses the extension's `Series`, and the native calls return it directly, so a call such as `s.abs()` runs the native method with no Python wrapper in between (`s.abs()` 252 ns to 46 ns and `df["x"]` 228 ns to 82 ns, in `benchmarks/python/call_bench.py`).
  `Series._native` is gone.
  The extension types are `Series`, `DataFrame`, `LazyFrame` and `TraceViewer`, without the underscore, can be subclassed, and `_bind_types` tells the native calls which Python subclass to build.
  `s.name = "x"` renames that one Series in place, as pandas and polars do, and the values stay shared with the Series it came from.
  `Series` cannot be subclassed from user code: `Series.__new__` raises a `TypeError` for a subclass, because the native calls build the class that `_bind_types` bound and would hand back a plain `Series`.
  The native types take weak references.
- Make the native `DataFrame`, `LazyFrame` and `TraceViewer` the public classes too: the Python classes subclass them and keep their state in slots, so a native call returns the public class and the per-call Python wrapper is gone (`df.head()` 523 ns to 340 ns and `df["x"]` 224 ns to 58 ns, in `benchmarks/python/call_bench.py`).
  Methods that keep the type return `Self` in the `.pyi`, `df["x"]` is a `Series`, and `tests/python/typing/frames_typed_sample.py` and `frames_typed_wrong.py` pin it for `ty` and `mypy --strict`.
  `TraceViewer` no longer subclasses `LazyFrame`: it holds its plan and `tv.lazy()` returns it, and every method it had still works.
  `DataFrame._replace` and `share` and `LazyFrame._replace` and `share` are the native in-place rebind and shallow copy; a kernel with the GIL released reads a copy of the frame taken under the GIL.
  `DataFrame.column_index`, `partition_id` and `TraceViewer.register_materialized` take keyword arguments natively.
  `DataFrame`, `LazyFrame` and `TraceViewer` copy with `copy.copy` and `copy.deepcopy` as a shallow share, since their values are immutable; `DataFrame[...]` is not subscriptable (it never was), and a typed frame comes later as `TypedFrame[Schema]`.
  `DataFrame.__contains__(name=)` and `__getitem__(key=)` no longer take a keyword, as they are native slot methods, and pandas does not take one either.
  Add `scripts/gen_trace_lazy.py`, which writes `python/dftracer/utils/_trace_lazy.py`, and a test that fails when the file is stale.
- Make the `Series` operators native: `+ - * / // % **`, their reflected forms and the six comparisons are the extension's number and rich-compare slots, so `s + 1`, `1 + s` and `s == t` run no Python method (`s + 1` 183 ns to 73 ns, `s == 1` 179 ns to 73 ns and `s < t` 786 ns to 89 ns on 4 rows).
  `count`, `arg_min`, `arg_max`, `nunique`, `skewness`, `kurtosis`, `product`, `mode` and twelve string, hash and list methods are rows of the API table, and `scripts/gen_pyi.py` and `tests/python/test_api_table.py` keep their declarations, the `Expr` methods and the list of hand-written methods in step with it.
- Type `Series` by its element: `Series[int]` is a column of integers, covariant, so a `Series[bool]` is a `Series[int]`.
  A comparison is a `Series[bool]`, an arithmetic op follows promotion (`Series[int] + float` is a `Series[float]`), `cast` and `astype` on a `DType` member give the element type of that member, a method that keeps the type returns `Self`, and `.str` reads on a `Series[str]` only.
  `Series([1, 2, 3])` is a `Series[int]` to mypy, and `tests/python/test_series_typing.py` holds the typed sample and the wrong-use fixture.
- Raise the typed errors from the native `Series`, `DataFrame` and `LazyFrame` calls, chosen by the error code, in place of a bare `ValueError` or `RuntimeError`.
  An invalid-argument error is a `DFTUtilsValueError`, which is also a `ValueError`, so `except ValueError` still catches it.
  A not-found error, such as an unknown column on an eager or a lazy op, is a `DFTUtilsNotFoundError`, which is also a `KeyError`, with the plain message as its `str`.
  These are no longer a `ValueError`: a lazy op error that is not an invalid-argument error (a not-found error was a `ValueError`, now a `KeyError`; any other code is its own `DFTUtils*Error`, a `RuntimeError`), and an eager op that threw a generic `std::exception` (now a `DFTUtilsError`).
  `df.group_by("a", "bogus:b")` raises a `DFTUtilsValueError` in place of a `KeyError`, and a failed Arrow import, an index build and a runtime call raise their own `DFTUtils*Error`.
- A `jit` plugin built into the cache is now `<cache>/<digest>/<plugin name>.so`, and a plugin that has a `jit.Store` loads under its class identity, so its store namespace stays the same when its body changes.
- `jit.Str` is a `str` to a type checker, so `Set[str]` and `Dict[Tuple[int, str], int]` take `e.name`.
- Replace `jit.config(jit.i64)` with the dataclass config above, read as `self.config.threshold` in place of `self.threshold`, and declare a `jit.record` as monoid values (`count = jit.count()`) in place of annotations (`count: jit.count`), so a type checker accepts both.
- The distributed code is its own library, `libdftracer_utils_comm`, in `src/dftracer/utils/comm/` under `dftracer::utils::comm`, and `libdftracer_utils_runtime` no longer links MPI. The ctest `comm_no_upward` keeps `comm` below `store`, and `mpi_only_in_comm_mpi` keeps `<mpi.h>` inside `comm/mpi/`.

- `dftracer_server` keeps its viz summary in the index store of each trace folder, namespace `server.viz_summary`, in place of one `.dftviz_summary` file per index directory. A restart serves it from the store, and a changed, added or removed trace file makes the server compute it again. An old `.dftviz_summary` file is ignored.
- `dftracer_server` caches its viz responses in the index store of each trace folder, namespace `server.viz_result`, so a restart serves a response that a trace file did not invalidate without scanning the trace. A response key carries the folder's fingerprint, so a changed file misses.
  One scope per folder holds both namespaces, and an added or removed trace file drops that scope, with the summary and every response of the folder in it.
  `DFTRACER_UTILS_VIZ_RESULT_STORE_BYTES` bounds the stored responses of one folder, 64 MiB by default, and a folder that would pass it drops its stored responses and starts again.
  A request no longer waits for the store, because the index writes each response in a task of its own and flushes the store once when the index shuts down.
- The plugin loader accepts an ABI version whose major matches the host's and whose minor is no greater than the host's; the patch is ignored. A refusal names both versions and the rule.
- Plugin-to-host records are accepted when ``struct_size`` is between the size of that record before its trailing optional callbacks and the host's ``sizeof``; the host copies the declared prefix into a zeroed host record and reads only that copy, so an omitted optional trailing callback reads NULL. The plugin ABI header hash is removed: ``dftu_plugin_info`` has no ``abi_hash`` field, ``abi_version.h`` defines no ``DFTRACER_UTILS_PLUGIN_ABI_HASH`` macro, and the Python extension no longer exports ``PLUGIN_ABI_HASH``. The JIT cache keeps its header fingerprint.

- The plugin SDK's host and config calls are typed: `Host::run_op` gives a `Column`, `run_op_frame` an `OwnedDataFrame` and `run_op_aggregate` a `dataframe::Scalar`; `find_op` gives an `OpRef` and `register_op` takes an `OpSpec`; op arguments come from an `OpArgsBuilder` whose string lists take `dstd::CStringView` spans; `publish` and `consume` take and give a `dstd::ByteView`; `Writer::chunk` takes a `dstd::ByteView`; `Sketch::result` gives `Quantiles`; `log` takes a `LogLevel`, `spawn` a `JobWhere`, config keys a `ValueKind` and aggregates an `AggOp`. A fold's `step` takes a `FrameView`, and a transform or frame `finalize` gives `dstd::Result<OwnedDataFrame>`. `Config` and `ConfigValue` read a plugin's options with no raw `dftu_value`.
- The plugin SDK's I/O, file-system, file-I/O and format calls are typed: every path is a `dstd::CStringView`, a buffer read in is a `dstd::MutableByteView` and a buffer written a `dstd::ByteView`, and a list of paths a `dstd::Span<const dstd::CStringView>`. `Fs::fstat` and `statx` fill an SDK `Stat` and `Statx`, `stat_batch` fills `FileStat` and `StatStatus` spans and refuses spans shorter than the paths, `find_all` takes `FindOptions`, `PathEntry::kind` gives a `FileKind`, `openat2` takes an `OpenHow`, and `GzWriter::close` gives a `GzSummary`. `co_await` on an `AsyncOp` or `AsyncAll` gives a `dstd::Condition`, and `FormatWriter::value` takes text up to its end, so a `char` buffer writes its string, not its whole size. A C++ plugin names no `dftu_*` type, call or `DFTU_*` constant.
- A plugin that declares a config key with an empty name fails to load with an error that says so. Before, the SDK ended the key list there and dropped every key after it.
- The plugin SDK's source, node and file-format views are typed: `SchemaBuilder::add` takes a `TypeId` and an optional `TimeUnit`, which a Timestamp, Time or Duration column reads in; `ExprView::as_compare` gives a `CmpOp` with a `ScalarView` literal, `as_str_pred` a `StrPredOp`, `as_is_in` a `ColumnView`, and `as_logical` a `LogicalOp`; `JoinView::how` is a `JoinHow` and `ScanResult::filters` a `dstd::Vector<Pushed>`, one vote per filter. `SchemaView` reads the names, types, nullability and time unit (`unit`) back out of a schema, and `OpArgs::size()` gives a node its operand count. `FileFormat::probe` takes a `dstd::ByteView` head and a `dstd::CStringView` path, `BindView::types` is a `Vector<TypeId>` and `BindView::options` the SDK `Config`, and `SinkWriter::write` takes a `FrameView`, so a C++ plugin author writes no `dftu_*` type, call or `DFTU_*` constant outside the entry point.

- `OwnedLazyFrame` in `dftracer/utils/plugins/result_registry.h` keeps its handle private, with `get()` and `release()` like `OwnedDataFrame`, and the `AggCol` builders name their columns with a `dstd::CStringView`, so no plugin reads the SDK's own field layout.
- Move Arrow conversion and IPC out of the core libraries into the arrow library (`dftracer_utils_arrow`, `dftracer/utils/arrow/arrow.h`), which the Python module and the tests link: `Series::to_arrow`, `Series::from_arrow`, `DataFrame::to_arrow`, `DataFrame::from_arrow` and `DataFrame::to_ipc` become `arrow::to_arrow`, `arrow::series_from_arrow`, `arrow::dataframe_from_arrow`, and IPC through `arrow::Writer` and `arrow::Reader`.
- Remove the CMake options `DFTRACER_UTILS_ENABLE_ARROW` and `DFTRACER_UTILS_ENABLE_ARROW_IPC`; the arrow library always has IPC.
- Remove the plugin Arrow service `dftu.svc.arrow`, the result kind `DFTU_RESULT_KIND_ARROW`, `ViewScannerBatch::to_arrow` and `ComparisonOutput::to_arrow`.
- Remove `AggregationConfig::output_format`, `FORMAT_JSON`, `FORMAT_ARROW`, `is_valid_format` and `supported_formats_str`, which nothing read.
- Remove `dftu_plugin::bytes` and `dftu_plugin::reclaim`, and `dftu_cursor_vt::bytes` and `dftu_cursor_vt::reclaim`.
  A slice reserves its bytes through `dftu.svc.memory` and sets a reclaimer there, in place of reporting a size for the host to ask about.
  A Slice no longer gets slots from `bytes()` and `reclaim()`, and a source cursor no longer gets them from `resident_bytes()` and `reclaim()`.
- Pass a `const dftu_plugin_host*` to `dftu_source_vt::scan` and `dftu_node_vt::open`, and to `scan` and `open` in the SDK source and node classes.
  The host stays valid until the cursor is destroyed, and it serves the services of a fold callback's host that need no scan.
  A cursor reserves memory through it, charged to the pool of its plugin, and its reclaimers run between two `next()` calls when another holder of the budget asks the pool for bytes.
  The host releases and logs a reservation that a cursor leaves.
- Rename the C API headers `abi.h` to `c_api.h` in `common`, `dataframe`, `duql`, `runtime`, `store`, `trace/index` and `trace/views`.
  Move the aggregate-over-expression C declarations from `dataframe/agg_expr.h` to the new `dataframe/c_api_agg_expr.h`.
  The plugin headers `plugins/abi.h` and `plugins/abi/` keep their names.
- Make the coroutine library throw and catch nothing.
  A task ends with a `dstd::Error` by `co_await coro::fail_with(error)`, or by `co_await` on a `dstd::Error` or an error `dstd::Result`.
  `co_await` on a task that ended with an error ends the awaiting task with the same error, and `Scope::join`, `when_all`, `when_any` and `SpawnFuture` do the same for the first error of their children.
  `coro::settle` gives the error as `Settled::error` without ending the caller, and `RetryPolicy::should_retry` takes a `const dstd::Error&`.
  `Runtime::block_on`, `sync_wait` and `TaskHandle::get` give the error as a `dstd::Result`.
  An exception that escapes a coroutine ends the process.
  `common::error_of_exception`, `common/exception_helpers.h`, `Generator::has_exception` and `AsyncGenerator::rethrow_if_exception` are removed, and the ctest `no_exceptions` always runs.
- Make the plugin SDK throw and catch nothing, and compile its headers with `-fno-exceptions`.
  `plugins::Error` and `AsyncOp::nothrow` are removed.
  A call that can fail returns a `dstd::Result`, and the callbacks of a Slice, source, cursor or node return one too, or `void` where they cannot fail.
  `co_await` on a `dstd::Result`, or on an `AsyncOp` whose call or task failed, ends the `Task` with that error.
  `co_await op.expected()` gives the failure as a `dstd::Result` instead, `co_await fail(error)` fails a `Task`, a Slice that can fail at creation provides `static dstd::Result<Slice> from_config(const Config&)`, and `DFTRACER_UTILS_TRY` and `DFTRACER_UTILS_TRY_VOID` in `dstd/result.h` return the error from a plain function.
  A callback that throws terminates the process.
  The ctest `plugin_no_exceptions` compiles every SDK header and three example plugins with `-fno-exceptions -Werror`.
- Remove `dftu_task_run`; run a task with the host task service `run` instead.
  `dftu_runtime`, `dftu_default_runtime`, `dftu_runtime_new` and `dftu_runtime_free` move from `coro/abi.h` to `runtime/abi.h`, and the `coro` and `runtime` libraries hold no plugin task type.
  `AbiTask` and `AbiContext` move to `dftracer::utils::plugins`.
- Add the ctest `lower_no_plugins`, which fails when a library below `plugins` includes a `plugins/` header.
- Register providers and plan nodes through the plugin host tables only.
  `dftu_provider_register` and `dftu_node_register` are removed from the dataframe C ABI, and the dataframe library keeps C++ interfaces for both, so it knows no plugin type.
  The `dftu_source_vt`, `dftu_cursor_vt` and `dftu_node_vt` tables, and the structs that only they use, move to `plugins/abi/providers.h` and `plugins/abi/nodes.h`.
  Registration refuses an empty name as well as a NULL one.
  A plan now keeps the node or provider it names alive, so `dftu_node_unregister` and `dftu_provider_unregister` no longer free state that a plan still holds.
- Add `run` to `dftu_svc_task`: it blocks until a task ends, frees it and returns its status, `DFTU_END` included, with the message on the ctx.
  The plugin SDK node input cursor uses it, and the factory host serves it.
- Keep a plugin library loaded while a node, provider, index extension or op from it, or a plan or cursor that holds one, is alive.
  Unloading a plugin removes its names at once; the plugin's own `destroy` runs after the last holder is gone and the library is closed after it, where it was closed at unload before and a plan could call into unmapped code.
- Add 26 calls to the plugin `dftu.svc.io` table: `unlink`, `rmdir`, `rename`, `mkdir`, `fadvise`, `linkat`, `symlinkat`, `socket`, `connect`, `bind`, `listen`, `shutdown`, `sendmsg`, `recvmsg`, `poll_add`, `statx`, `fallocate`, `sync_file_range`, `splice`, `openat2`, `getxattr`, `setxattr`, `fgetxattr`, `fsetxattr`, `tee` and `pipe`, each with an `Io` method in the SDK.
  The host copies paths, names and socket addresses when called, and an errno of ENOSYS, EOPNOTSUPP or ENOTSUP fails a task with `DFTU_E_UNSUPPORTED`.
  The calls that need Linux return `DFTU_E_UNSUPPORTED`, with no task, on another platform.
- Add the plugin service `dftu.svc.fs` over the runtime file system layer: `read_file`, `write_file`, `read_at`, `write_at`, `list_dir`, `find_all`, `exists`, `create_directories`, `remove` and `stat_batch`.
  A listing is a host-owned `dftu_path_list` read through `list_count` and `list_entry` and freed with `list_free`; the SDK has `Host::fs()` with `Fs`, `PathList` and `FileBytes`.
  A path that `stat_batch` cannot stat fails its own slot with `DFTU_E_NOT_FOUND` or another status and not the task.
- Redesign the plugin C ABI around a status and a call context.
  Every plugin callback takes a host-owned `dftu_ctx*` first and returns a `dftu_status`; a callback reports an error with `dftu_fail` and polls `dftu_check` in a long loop.
  Every host service call takes the caller's `ctx` first and gives results through out-parameters.
  Every table starts with `struct_size`, and service ids carry no `@N` suffix.
  The host names the plugin and the callback in the error the caller sees.
- Make the plugin loader read a `dftracer_plugin_info` record before it runs any plugin code.
  A plugin defines it with `DFTU_PLUGIN_INFO`; the loader refuses a plugin that exports none, or whose ABI version it does not accept, and names the plugin in the message.
- Replace `dftu.svc.coro` and `dftu.svc.compose` with `dftu.svc.task`: jobs (`job`, `sleep`, `then`, `all`, `any`, `map`) with typed values, and tasks (`start`, `spawn`, `drive`, `status`, `discard`).
  `drive` takes a `release` callback that the host calls once when the drive ends, and `discard` gives back a started task that the plugin will not hand to the host.
  `DFTU_JOB_BLOCKING` gives the run permit back while a leaf blocks, and the first error of `all` or `map` cancels the rest.
  The C++ SDK has typed `Job<In, Out>` in `plugin/job.h` in place of `Op`.
- Make the C++ plugin SDK catch every exception at the ABI boundary and give it to the host as an error with its `what()` text.
  A call the host refuses throws `plugins::Error` out of the plugin code, `Host` carries the call context, and `make_plugin` and `plugin(...).build` take the context and give the descriptor through an out-parameter.
- Make a plugin index extension whose `step` fails store nothing for the file, where it stored a file payload built from the chunks it did see and pruned matching rows.
- Make the store and index C functions return a `dftu_status`, give results through out-parameters and take a caller-owned `dftu_error` as the last parameter.
  The error message is owned by the caller and freed with `dftu_error_release`, the enums are `int32_t` typedefs, and `dftu_store_pack_options` and `dftu_indexer_options` start with `struct_size`.
- Make `to_pandas()` give pandas' nullable dtypes (`Int64`, `boolean`, `string`) for integer, bool and string columns that hold nulls, and `category` for a dictionary column, so `DataFrame.from_pandas(df).to_pandas()` keeps those dtypes.
  `from_pandas` keeps a `category` column with string categories as a dictionary column, in category order where pyarrow is installed, and `to_pandas` gives back only the categories that are used.
- Make `to_polars()` give `Categorical` for a dictionary column and `from_polars` read a `Categorical` or `Enum` column as a dictionary column, where both were plain strings.
- Make `DataFrame::names` a `dstd::Vector<dstd::SharedString>`, so reading a column by name copies a reference count and does not copy the name.
- Pick the best vector instructions of the CPU at run time for `str_len_chars`, `str_len_bytes`, `to_lowercase`, `to_uppercase` and the UTF-8 check of duql patterns, which were built for SSE2 only on x86.
  `str_len_chars` of 1M short rows takes 2.2 ms where it took 12.9 ms on an AVX-512 x86 CPU, and the other three are 10 to 15% faster.
- Read only the parts of the index store that a query needs.
  A query no longer reads each 16 MiB window of a store file that holds two of its extents, and it reads 4 KiB for the page table of an extent where it read the whole extent.
  On Lustre, with an index of 14,117 traces (318 MB), a no-op `dftracer_index` reads 20 MB where it read 349 MB, `dftracer_view -q 'take 20'` 15 MB where it read 202 MB and a group-by 105 MB where it read 654 MB, cold and warm, with the same or a shorter run time.
- Read each gzip trace once when it is indexed.
  The index build no longer scans a file for member headers before it inflates it: each worker takes a range of the file (half the checkpoint size in compressed bytes, at least 4 MiB; `DFTRACER_UTILS_INDEX_RANGE_BYTES` sets it) and finds its first member header in the bytes it reads to inflate.
  A build of 97 DFTracer traces (343 MB) read 803 MB of them before and reads 366 MB now, with the same index.
  A range that starts inside a member reads the end of that member a second time, up to half a member per range, so a file of 2 MB members reads about 9% more than its size; members longer than a range are read once and inflated one after another.
  The memory of a build is bounded by the plaintext of a range (a quarter of the checkpoint size, at least 4 MiB) and is no higher than before at 8, 16 and 32 MB checkpoints.
  A member that cannot be read, or bytes that are no gzip between members, now lose only those bytes: the build warns, looks for the next member header and goes on, where it dropped the rest of the range before.
  A read or a view of a damaged file skips the text that the damage cut, so the line count of its index can be a few lines higher than the lines a read returns.
- Back blocks of 2 MiB and more with transparent huge pages on Linux, where the system gives them on request (`transparent_hugepage` set to `madvise`).
  A large new buffer then faults in 2 MiB pages, not 4 KiB ones: a YAML read bench made 13 times fewer page faults and ran 15 to 22% faster.
  Smaller blocks keep 4 KiB pages, so their resident size does not change.
- Read YAML with our own reader and drop rapidyaml, with its c4core, fast_float and debugbreak.
  A line index built with Highway reads block YAML (maps, sequences, quoted and plain scalars, block scalars `|` and `>`, scalars of several lines, top-level scalar documents, comments, `---` and `...` markers), and a recursive parser reads flow collections (`[...]`, `{...}` and JSON) into a flat tape of 12-byte entries, with no tree.
  Every text goes through the line index, so no text is left to a second reader.
  A flow collection may go over several lines, and `yaml::Records` never cuts inside one.
  A parsed text holds its own bytes plus 0.82 times its size for a list of short records and 0.58 times for a deeply nested map (measured on 100 MB inputs).
  A leading byte order mark is skipped.
  An error is `file:line:col: reason`, with the line counted over the whole text, also in `yaml::Records`.
  The reader is checked against the cases of the YAML test suite and a fuzz that writes random trees as text and reads them back.
- Refuse anchors (`&`), aliases (`*`), tags (`!`), complex keys (`?`), directives (`%YAML`, `%TAG`) and null keys in YAML, with an error that names the feature.
  A `<<` key is a plain key: there is no merge.
  A mapping with a container as a key is refused.
  `yaml::Writer` no longer writes an explicit `? key` for a key of more than 1024 bytes: such a key is now a caller bug, fatal in a debug build, as YAML limits an implicit key to 1024 characters.
- Rename the library `libdftracer_utils_io` to `libdftracer_utils_fileio` and `libdftracer_utils_utilities` to `libdftracer_utils_domains`, with their pkg-config files.
  A program that links the `dftracer_utils` target needs no change.
- Move the public index headers from `dftracer/utils/index/` to `dftracer/utils/trace/index/` and their namespace from `dftracer::utils::index` to `dftracer::utils::trace::index`.
- Move the C++ namespaces of the domain code under `dftracer::utils::domains`, such as `domains::analysis::comparator`, `domains::workload::genesis` and `domains::server`.
- Link Vectorscan only into the duql library, so the trace library no longer holds a second copy and is about 10 MB smaller on Linux x86-64.
  On Linux the duql library no longer exports the `hs_*` symbols of Vectorscan.
- Rename the `morsel_rows` parameter of `LazyFrame.collect` and `LazyFrame.stream` to `batch_size` in C++ and Python.
- Report the first input error of the duql builder from `Pipe::text()` and `Source::text()`, which now give a `dstd::Expected<dstd::String, DuqlError>`, in place of raising `std::invalid_argument`.
  A `Col`, `Pipe` or `Source` keeps the first error of the calls that built it, such as a number that is not finite or a `bucket` with a bad mode.
  `error()` reads it.
  `View::duql(pipe)` returns the error for a `Pipe` that holds an error.
- Remove `duql::parse_or_throw`.
  Use `Query::from_string`, which gives a `dstd::Expected`, or `try_parse`.
- Return a `dstd::Result` from `duql::load_macros` and `duql::path_macros` in place of raising.
  The Python `duql.load_path` still raises the same exception.
- Build the json and duql libraries without exceptions.
- Build the io library without exceptions.
- Build the trace library without exceptions.
  Python keeps its exception types.
- Build the utilities library and the command line tools without exceptions.
- Make the viz server answer 400 for a bad query, such as an invalid argument or a duql parse error, and 500 for I/O and internal errors, where every failure answered 500 before.
- Make a command line tool that fails print `Error: <message>` and exit with 1 in cases where it aborted before, such as an index folder that cannot be written.
- Make `dftracer_stats` in detailed mode stop at the first error and exit with 1.
- Print NaN and infinite values as `nan`, `inf` and `-inf` on every platform.
- Return a `dstd::Result` from `View::export_would_bootstrap`, `collect_would_bootstrap`, `materialize_dir`, `register_materialized`, `mv_source` and `reconstruct_if_cached`, and from `rows<T>`.
- Return the first builder error of a `View` from every terminal, in place of raising.
  A view over files of more than one record schema returns the mixed record schema error, in place of falling back to the dftracer schema.
  The server `columns` endpoint answers 400 when the column info fails.
- Return a `dstd::Result` from the `Indexer` and the index build, in place of raising.
- Return a `dstd::Result` from `Source::scan` and `Source::open_batch`.
  A source that cannot open a scan returns the error and the collect ends with it.
  A `Cursor` must not throw from `try_next`, `out_names` or `resident_bytes`.
- Build the dataframe library without exceptions.
  The C ABI functions of the library return the same null or error values as before.
- Write the `DFTRACER_UTILS_LOG_*` macros and `DFTRACER_UTILS_TRACE_SCOPE` with `dstd::fmt` formats (`{}`, `{:x}`, `{:.2f}`) in place of printf formats, checked at compile time in every build.
  A message over 1536 bytes ends in `...`.
  The log line layout is unchanged, and a `bool` in a message now prints as `true` or `false`.
- Return a `dstd::Result` from `LazyFrame::collect`, `stream`, `open_cursor` and `reduce_specs`, from `collect_all` and `LazyResult::collect`, and from `DataFrame::join`, `compare_agg` and `to_ipc`, in place of raising.
  A `LazyFrame` builder keeps its first error: `status()` reads it, and every terminal returns it.
  `Cursor::next` gives a `MorselResult`.
  `Cursor::try_next` is now `bool try_next(std::int64_t max_rows, MorselResult& out)` and does not throw.
  A source that raises from `next` or `collect_batch` ends the collect with an error.
  A source that raises from `narrow` or `reclaim` is logged, the call counts as nothing done and the collect goes on.
  A source that breaks the batch or planning hook contract ends the process.
  Python keeps its exception types.
  `take` now raises `DFTUtilsValueError`, a subclass of `DFTUtilsError`, for a bad index.
- Require a `noexcept` function for `dstd::add_shutdown_hook`, so a hook that can raise does not compile.
- End a cancelled task at its wait, in place of resuming it with an exception: the frame is destroyed, and the stop moves up to the first scope or combinator.
  `coro::with_timeout`, `coro::with_deadline`, `JoinSet::next_within` and `coro::with_item_timeout` give a `dstd::Result` with `dstd::Condition::Timeout` in place of raising, and the `timeout` adaptor of a stage gives a `dstd::Result` of its output.
  `coro::throw_cancelled`, `coro::is_cancelled` and `coro/cancelled.h` are gone.
  A misuse of a coro API, such as a zero size or limit, a join of the scope that spawned the task, or a spawn into a closed scope, ends the process in place of raising.
- Return a `dstd::Result` from `CoroTask::get()`, `coro::sync_wait`, `Runtime::block_on`, `TaskHandle::get()` and `TypedTaskHandle::get()`, and from `Runtime::parallel_for` and `parallel_reduce`.
  A task that a stop ended gives `dstd::Condition::Cancelled`, and a runtime that was shut down refuses a task with the same condition, in place of raising.
  A task that returns a `dstd::Expected<V, dstd::Error>` gives that type itself. An exception that a task raises is still rethrown from the calling code.
  `GroupBy::transform` and the eager group-by methods, and the sync wrappers of `TraceReader`, give a `dstd::Result` too.
  The runtime and store libraries are built without exceptions.
  A `Runtime` built with a bad memory setting in the environment ends the process with the error in place of raising.
- Remove the `bool` conversion of a `dstd::Expected<bool, E>`, so `if (result)` no longer compiles where it reads as the value and means the state.
  Test the state with `has_value()` and the value with `*result`.
- Make `dftu_op_desc::fn` a function pointer of the new type `dftu_op_generic_fn` in place of `const void*`, so a plugin author casts an op function with `(dftu_op_generic_fn)&my_op`.
- Make `dstd::Expected` our own class and remove the vendored `tl::expected`.
  `value()` and `error()` on the wrong side now end the process with the message and a stack trace, where `value()` threw before.
  A build with `DFTRACER_UTILS_DEBUG` also ends the process when an error result is destroyed unchecked.
- End the process with a message when memory runs out or an allocation would pass `DFTRACER_UTILS_MEM_HARD_LIMIT`, where it threw `std::bad_alloc` before.
  Python gets an abort in place of `MemoryError`.
- End the process when `dstd::any_cast` gets the wrong type or an empty `dstd::Function` is called, where they threw before.
- End the process with a message that names `DFTRACER_UTILS_IO_BACKEND` when it holds a bad value, or when the I/O thread pool cannot start a thread, where creating a `Runtime` threw before.
- Raise a Python error from `Runtime.set_timeout` and `Runtime.set_default_task_timeout` on a runtime without a watchdog, where the C++ call threw before.
- Make an index of system counter traces up to 57% smaller, because each counter value is stored as a change from the last value of the same counter.
  Rebuild indexes made by an earlier version.
- Store index extents on 8-byte boundaries in place of 4 KiB, so an index of a small trace set is up to 23% smaller.
- Add `DFTRACER_UTILS_STORE_DIRECT_READS` to read the closed files of an index store past the page cache (`O_DIRECT` on Linux, `F_NOCACHE` on macOS).
  It is off by default, and a file system that refuses direct reads gets cached reads.
- Make the plugin call `duql_matches` about 35% faster per row, because the columns a query reads are found once per frame.
- Make an index build allocate about 7% fewer blocks, because the exact quantile and distinct id lists grow in large steps.
- List the call sites with the most allocations at exit when `DFTRACER_UTILS_MEMORY_DEBUG=1` is set.
- Run a task on the process runtime when a thread that is not a runtime worker calls `get()` on it, such as `view.collect().get()` in `main()`, so the work uses every core instead of the calling thread only.
  Bind a `coro::RunLoop` with `coro::CurrentScope` to keep the work on the calling thread.
  The row order of a result with no sort is not fixed.
- Make a trace scan with no index answer up to about 25% faster when the query reads no args, because each line is decoded once and the args are not decoded.
- Grow the index in place when a trace grows by whole gzip members, so a refresh costs only the new members.
- Lower the default of `DFTRACER_UTILS_MEM_DECAY_MS` from 1000 to 200, so the heap gives idle pages back sooner.
  Peak memory of the index, stats, view and comparator tools falls 6% to 15% on a 1 GB trace set, with no change in run time above noise.
- Add `runtime::fs::exists`, `create_directories` and `remove`, and run the per-file checks of the indexer and the tools off the worker, so they no longer hold a worker thread.
- Let another worker run while a store commit or a spill file blocks on slow storage, so a tool keeps its full parallelism during long writes.
- Give each executor worker its own io_uring ring for positional reads and writes, `close` and `fsync`.
  With io_uring these run 1.25 to 4.5 times faster in `io_bench`, and a read of cached data now ends with no thread woken.
  The backend needs Linux 5.6 or later and otherwise falls back to the thread pool.
- Add the async operations `runtime::io::statx`, `unlink`, `rmdir`, `rename`, `mkdir`, `fallocate`, `fadvise`, `sync_file_range` and `splice`, and run `ftruncate` on the ring too.
  The io_uring backend asks the kernel which of them it has and sends the rest to the thread pool, and `runtime::fs::stat`, `exists`, `remove` and `create_directories` use them on Linux.
- Add the async operations `runtime::io::linkat`, `symlinkat`, `socket`, `connect`, `bind`, `listen`, `shutdown`, `sendmsg`, `recvmsg`, `madvise`, `waitid` and `poll_add`, and on Linux `openat2`, `getxattr`, `setxattr`, `fgetxattr`, `fsetxattr`, `tee`, `pipe`, `futex_wait`, `futex_wake`, `futex_waitv`, `epoll_ctl` and `epoll_wait`.
  The io_uring backend runs each on the ring when the kernel has its opcode and on the thread pool otherwise.
  Where it cannot end a wait on the pool, the doc comment of the call says so.
- Add `DFTRACER_UTILS_IO_URING_SQPOLL_IDLE_MS` to run the io_uring backend with a kernel polling thread.
  It is off by default, because that thread holds a core while I/O runs.
- Run coroutines on one queue per worker, with stealing between workers, in place of one shared queue.
  Scheduling is faster, most of all when one worker wakes another task, and an idle pool no longer wakes on a short timer.
- List every `*.gz` file when a tool, an indexer or a viewer discovers the traces in a folder, and no longer list plain `.pfw`, `.jsonl` or `.ndjson` files, so gzip them first; the Python constant `TRACE_FILE_PATTERNS` becomes `INPUT_FILE_PATTERNS`.
- Report a failed tool run with the error of the failed step, in place of `[EXECUTION] Pipeline execution failed`, and print `Run failed:` in place of `Pipeline failed:`.
- Rename the `Pipeline` group of tool flags to `Runtime`. The flags stay.
- Make `dftracer_split` and `dftracer_gen_fake_trace` stop the other jobs at the first failure, in place of letting them finish.
- Make `--watchdog-task-timeout` cover the whole run of a tool, since a tool now runs as one task.
- Stop the children of a `coro::Scope` when one fails or when the task that holds the scope is cancelled, so they end at their next wait in place of running on.
  Pass `coro::ScopeMode::Supervisor` to keep the old behavior for a child error.
- Add the stream operations `coro::take_until`, `chunks`, `zip`, `fold`, `merge`, `flat_map_merge` and `buffer`.
  Each stream operation in `coro/stream.h` also comes as an adaptor, such as `gen | coro::filter(pred)`.
- Make a stop or a cancel of a task end a wait inside an `AsyncGenerator` that it reads, and let `this_coro::executor` work in a generator.
- Remove the `AsyncGenerator` methods `map`, `flat_map`, `filter`, `take` and `concat` and the operators `>`, `>>` and `|`.
  Use the functions and adaptors in `coro/stream.h`.
- Give the sync `coro::Generator` the same stream operations and adaptors, and make its `next` return the item, as on `AsyncGenerator`.
  This replaces `Generator::value`.
- Add `coro::to_async`, which turns a `Generator` into an `AsyncGenerator`.
- Add the awaitable `coro::parallel_for`, `parallel_reduce` and `parallel_scan` with a choice of static or dynamic `Partition`.
- Add `coro::JoinSet` with `with_join_set`, and `as_completed`.
- Add `coro::map_ordered` and `map_unordered`, and the stage adaptors `ordered` and `unordered` with a token limit.
- Add `coro::StopSource`, `StopToken`, `with_stop_token` and `this_coro::stop_requested`.
- Wake a task parked on a channel, event, semaphore, mutex, sleep or future when the task is cancelled, so `when_any` and `SpawnFuture::cancel` no longer wait for the parked wait to end.
  The wait raises a cancelled error.
- Rename `coro::CoroSemaphore` to `coro::AsyncSemaphore`, and add `permit()`, which gives the units back on destruction.
- Raise timeouts from `coro::timeout` with the condition `dstd::Condition::Timeout`.
- Add `coro::sleep`, `with_timeout`, `with_deadline`, `retry`, `when_all_settled`, `first_success`, `parallel_map`, `parallel_map_unordered`, `AsyncEvent` and `AsyncLatch`.
- Make `coro::Channel` a lock-free ring on the send and receive fast paths, 3 to 60 times faster in `channel_bench` at 48 and 96 threads.
  Items of one producer now arrive in order.
- Make `coro::UnboundedChannel` a queue of linked blocks with one global order, 2.5 to 12 times faster in `channel_bench` on most rows, and 13% slower with 16 senders and 16 receivers on 48 threads.
  `make_unbounded_channel` no longer takes a size.
- Require a capacity of at least 1 for `coro::Channel`, where 0 meant unbounded, and add `coro::UnboundedChannel` with `make_unbounded_channel` for that case.
  `dftracer_replay --channel-capacity 0` is now an error.
- Install only our own `include/` tree: the headers of nanoarrow, flatcc, zstd, libdeflate, pcre2, yaml-cpp and doctest no longer reach the install prefix, nor do their pkg-config files, man pages and documentation.
  A static build still installs their archives and CMake configs, so a static consumer links as before.
- Rename the trait structs `dstd::awaitable_result_type` and `dstd::unwrap_coro_task` to `AwaitableResultType` and `UnwrapCoroTask`, the macros `DF_*` of the dataframe kernels to `DFTRACER_UTILS_*`, and the public fields of coroutine promise and shared-state types (`coro::PromiseBase`, `coro::CoroTask<T>::promise_type`, `coro::detail` completion states) to names without a trailing underscore.
- Rename C++ names to the word rules: getters lose `get_` (`Runtime::progress()`, `TaskExecutor::thread_count()` and `io_pool_size()`, `common::logger::level()`, `coro::timeslice_duration()`, `CoroScope::executor()`), `num_x` becomes `x_count` (`ExecutorConfig::thread_count`, `Host::writer(path, worker_count, gzip)`, the `slot_count` argument of `map_batches`), `x_len` and `x_length` become `x_size` (`dstd::path_detail::root_size`), `create_x` becomes `make_x`, and `dstd::get_type_name<T>()` becomes `dstd::type_name<T>()`. The C ABI, the Python API and the strings users type (CLI flags, JSON keys, environment variables) keep their names.
- Build a plugin against the installed headers without nanoarrow: `plugins/owned_arrow.h` holds the Arrow C structs and releases them itself.
- Add the ctests `public_headers` and `public_headers_compile`: the first checks `include/` against `cmake/public_headers.txt`, the second compiles each installed header alone, and the abi headers as C.
- Read each event in `collect_events` once on demand, in place of a second DOM parse of the line.
  A line whose bad part is never read now stays, and a read of real DFTracer lines takes no more time than one DOM parse.
  Records decoded by path, and the nested captures of a containment branch, read on demand too.
- Keep a JSON line whose syntax error lies in a part nothing reads, in the public fold callbacks and predicates (`Query::matches`) and in the reader path for queries with expressions: they read each line once on demand and check only what they read, where they dropped a line that failed a full parse. A line whose bad part is read is dropped. The reader path for plain field filters walks every field of a line, so it drops a line with a bad part anywhere: `{"name":"read","dur":}` is dropped for `name == "read"` and kept for `lower(name) == "read"`. A bigint counts the same way: unknown when read, and no reason to drop the line when not.
- Pass the callback of `ViewSession::fold` a `json::Value&`, read on demand from one parse of the line, in place of `const json::JsonValue&`. A fold that only counts reads no field, and several folds and predicates on one line share the parse.
- Hash every string with XXH3-64, in memory and in index, cache and store files.
  Existing indexes, caches and store data files are rebuilt, and the store skips old data files with a warning.
  Genesis run ids change.
- Take the inputs of every command line tool as positional paths, files and folders mixed, and remove `-d`/`--directory` and `--files`.
  `dftracer_index`, `dftracer_split`, `dftracer_gen_dlio_config` and `dftracer_server` take one folder and no files.
- Use `-f`/`--force` in every tool that forces a rebuild, where `dftracer_info` had `-f`/`--force-rebuild`.
- Use `-o`/`--output` in every tool that writes output, where `dftracer_gen_fake_trace` had `-o`/`--output-dir`.
- Use `-q`/`--query` for duql text in every tool, and remove `--duql`.
- Make replay sampling with a fixed seed pick the same events on every platform.
  The same seed now picks a different set of events than older versions did.
- Choose the `dftracer_info` report with `--report summary|detailed`, where `--query` did.
- Remove the short forms `-e`, `-H`, `-p` and `-s` of `dftracer_gen_fake_trace`, `-c`, `-n` and `-s` of `dftracer_split`, and `-l` of `dftracer_pgzip`.
  The tools keep `-h`, `-v`, `-o`, `-f`, `-q`, `-t`, and `-b` and `-p` of `dftracer_server`.
- Replace argparse with a small option parser in the command line tools.
  `--help` has a new layout, and a parse error prints a new message.
- Keep the most recent `task_history` finished tasks (1024 by default, `ExecutorConfig::task_history`) for `get_progress`, where the runtime kept all of them.
- Move the coroutine headers from `dftracer/utils/runtime/coro/` and the executor headers from `dftracer/utils/runtime/executor/` to `dftracer/utils/coro/`, in a new `dftracer_utils_coro` library that the runtime library links.
  The types are now in namespace `dftracer::utils::coro` (for example `coro::Executor`, `coro::TimerService`), and the I/O backend accessors moved from `coro::Executor` to the runtime's `TaskExecutor`.
- Move the coro and memory internals (frames, park registry, completion state, monitor, heap, manager and constants) to `detail/` headers and the `detail` namespace, which are installed but have no stability promise.
  `memory::Config`, `memory::configure` and the allocators stay public.
- Replace `Runtime::run_blocking(name, fn)` and `Runtime::scope(name, fn)` with `Runtime::block_on(task)`, which returns the task's value.
  Name a blocking task with `coro::named("name", task)`, and spawn children in `coro::Scope` (`include/dftracer/utils/coro/scope.h`) or `coro::with_scope(fn)` in place of `CoroScope`.
  An error of a spawned child is now raised by the scope's join, where `CoroScope` dropped it.
- Make `coro::when_all` and `coro::when_any` over a list take and return `dstd::Vector`, and make channels, spawn futures and generators use `dstd::Optional` and `dstd::SharedPtr`, so their memory counts in the memory budget.
- Make `TaskProgress::state` a `TaskState` enum and `TaskProgress::location` a `TaskLocation` enum with a `worker` index, where both were strings.
  Python's `Runtime.get_progress()` returns the same strings as before.
- Report a task submitted with `Runtime::submit` that raises as failed in `get_progress()`, where it was reported as completed, and count a failed task only in `failed`.
- Make the `common` helpers return dstd types: `string_format` and `human_bytes` return `dstd::String`, and `Env::get` and `parse_bytes` return `dstd::Optional`.
- Move the store, json, duql and plugin C++ APIs onto dstd types: the plugin SDK builder, configs and results take and return `dstd::String`, `dstd::Vector` and `dstd::Optional`.
- Build each `distributed_index` worker's files as a shard on node-local disk, then merge the shards into the index root by moving their data files.
  Shard sets live in `.dftindex/shards/` with `.dftindex/shards.json`, where they lived in `.dftindex-shards`.
- Make `consolidate_shard_set` merge the shards' data files instead of building the index again.
- Keep the query caches and materialized views inside the index root, in `.dftindex/rollups/`, `.dftindex/lookups/` and `.dftindex/views/`.
  A `.dftindex-cache` folder next to the traces is no longer used and can be deleted.
- Make `Index.find_stale_files()` return changed, added, removed and stale files from one snapshot of the index root.
  It no longer returns the `format_outdated` key.
- Start the I/O thread pool of the epoll, kqueue and thread-pool backends with one thread, and add a thread when a request waits and no I/O thread is free, up to the configured I/O thread count.
  An idle I/O thread above the first one stops after the elastic keep-alive. `DFTRACER_UTILS_ELASTIC=0` or an eager runtime starts the full pool, as before.
- Key the Python PID queries by trace path, because a file id is numbered per process and means nothing to a caller.
  `Indexer.query_file_pids` takes a trace path, and `query_all_file_pids` and `query_file_info` return a dict that maps each trace path to its PID set.
- Make `dask.assign_files_by_pid` take and return trace paths, and send a file with no recorded PID to the worker that the `zlib.crc32` of its path picks, so two processes give the same assignment.
- Size a runtime's I/O thread pool to its worker count when no I/O thread count is given, where it used to start one I/O thread per hardware thread.
  The CLI tools keep their `--io-threads` default of one per CPU core.
- Make `dftracer_comparator` skip the parts of each stored aggregation partial that it never reads, and give each side half of the executor threads when it aggregates both at once.
  A warm comparison of the 7,742 matrix traces at 96 threads takes 2.7 s where it took 3.8 s, with a quarter less CPU time, and the report stays the same.
  Its per-process pass keeps only the count and the duration and size sums of each row, so a warm comparison takes 2.0 s and peaks at 2.4 GB where it peaked at 3.5 GB.
- Make `dftracer_comparator` give each of its two index builds half of the executor threads when it builds both at once, where each planned for all of them.
  A cold comparison of the 7,742 matrix traces at 96 threads takes 20 s where it took 23 s, and peaks at 5.6 GB where it peaked at 8.1 GB.
- Make `dftracer_stats` read the per-file rows of a fresh index for every report except `detailed`, where it folded the aggregation of every chunk and wrote a rollup cache beside the index on the first run.
  A run now writes nothing.
  The time span ends at the end of the last event, and the duration count, mean and deviation use only the events that have a duration.
  A file whose rows are missing or not current still goes through the aggregation.
  The first `dftracer_stats` of the 7,742 matrix traces after a build takes 1.3 s where it took 4.6 s, and peaks at 0.3 GB where it peaked at 3.1 GB.
- Make `dftracer_event_count` read the count of a file from a fresh index and otherwise stream the file, where it built an index first, so it writes nothing and no longer takes `--checkpoint-size`.
  The first count of the 7,742 matrix traces at 96 threads takes 1.7 s where it took 15 s, and peaks at 1.4 GB where it peaked at 5.4 GB.
  A count from an existing index reads one total per file, where it decoded the statistics of every chunk, and takes 0.6 s where it took 2.4 s on the same traces.
- Make a scan of one file's rows in the index decode only the part of a stored segment that can hold them, where it decoded the whole segment that the rows of many files share, and read each file's string table in the task that reads that file.
  The first `dftracer_stats` of the 2,376 amg traces at 96 threads takes 5.7 s where it took about 150 s, and peaks at 1.5 GB where it peaked at 7.2 GB.
- Check whether the indexed files are fresh in parallel, where one thread checked them one by one, and decode only the index pages a point lookup needs.
  The fresh check of the 2,376 amg traces takes 1.4 s where it took 3.0 s with the index in the cache.
- Inflate a gzip member that grows past 8 MiB in 1 MiB pieces, where a build held whole members of up to the checkpoint size for each file in flight.
  A cold `dftracer_index` of the 2,376 amg traces at 96 threads peaks at 8.3 GB where it peaked at 11.2 GB, and the 7,742 minife traces at 3.5 GB where they peaked at 4.9 GB, in the same time.
- Write the aggregation rows of a file to the index in 4 MiB runs as the build closes its chunks, where a build held every row of each file in flight until the file ended.
  A cold `dftracer_index` of the 2,376 amg traces at 96 threads peaks at 5.2 GB where it peaked at 8.1 GB, in the same time.
  Sets of many small files peak a little higher: 3.7 GB where they peaked at 3.4 GB for the 7,742 minife traces and 2.0 GB where they peaked at 1.8 GB for the 14,117 minife traces.
- Count the memory zlib-ng, zstd and libdeflate allocate against the memory pool that uses it, so the memory budget and the memory reports include it.
- Bound the memory the allocator keeps after a free, so peak memory no longer grows with the thread count: past 1 GiB of freed but still backed pages, a thread that frees gives its idle pages back at once, and an idle worker or I/O thread gives back its own.
  A cold `dftracer_index` of the 8,404 minife traces peaks at 5.2 GB where it peaked at 6.8 GB at 224 threads, and at 3.1 GB where it peaked at 3.6 GB at 96 threads, in the same time.
- Count a pre-aggregated counter record as the events it stands for when a trace aggregation reads the trace, with its duration, size and offset statistics from its args, so the answer matches the index.
  A frame still counts its rows, so a pre-aggregated record is one row there.
  Percentiles, histograms, plans that decode records by path, time buckets on a plan with no schema and group keys on arguments the build did not keep still count it as one row.
- Store per-event column data in the index only for system counter rows (phase `C`, category `sys`), which makes the index much smaller.
  Quantile, offset and bin queries that the stored partials cannot answer now read the trace again.
  `AggQueryStats::chunks_from_columns` and `AggregatedTraces::chunks_from_columns` are gone.
  Group by file is answered from new per-file partials, so it needs no trace.
  Indexes built before this change rebuild their aggregation data once.
- Use less memory to build an index, because the aggregation data of each file is kept at its exact size with one copy less.
  File admission now counts these bytes, so a memory budget limits them too.
- Make the aggregation tier part of every index build, with one standard config: 1 s windows and the percentile sketches on. `dftracer_index`, the freshness paths of the views and the other tools, the C++ `Indexer`, the C ABI, Python, the distributed build and the comparator all build it, and a caller may pass a customized config but cannot turn the tier off.
  `require_aggregation` is gone from `IndexerOptions`, `ResolveAndBuildInput`, `ResolverInput`, the Python indexer and the batch indexer, and `comparison_index_config()` is gone in favor of `standard_aggregation_config()`.
  The Python `AggregationConfig` default interval is 1 s, as in C++.
- Keep counters, system samples and pre-aggregated (`ph:"A"`) records in the stored aggregation partials, so a group-by over every record kind reads only the index, and a query on the events phase no longer counts `ph:"A"` records.
  Indexes built before this change rebuild their aggregation data once.
- Keep per-thread time bins (count, duration sum and size sum per bin of the aggregation interval) in each stored aggregation partial, so `dftracer_comparator` reads its per-thread windows from the index and parses no trace when the index is warm.
  A warm comparator of the 8,404 minife traces against itself at 96 threads takes 11 s where it took 16 s and 87 CPU seconds where it took 505, the 7,742-trace set takes 59 s where it took 77 s, and the index build time and size do not change.
  Indexes built before this change rebuild their aggregation data once.
- Merge the rows of an aggregation query's file and chunk tasks into 64 stripes with a lock each, where every task merged into the result under one lock and the other workers waited on it.
  A cold `dftracer_comparator` of the 14,117 minife traces against itself at 96 threads takes 30.9 s where it took 35.9 s, and a warm one 16.8 s where it took 21.1 s.
- Keep the argument, field and schema-leaf lists of the events a scan buffers in shared chunks of their buffer, where each event held its own heap block for each list.
  A cold `dftracer_index` of the 14,117 minife traces peaks at 0.80 GB where it peaked at 0.92 GB at 16 threads and at 2.7 GB where it peaked at 3.1 GB at 96 threads, and takes 3 to 6% less CPU.
- Commit each worker's index write once it holds 4 MiB instead of 32 MiB, as fast and with a 3% larger index.
  A cold `dftracer_index` of the 14,117 minife traces peaks at 0.98 GB where it peaked at 1.4 GB at 16 threads, and at 3.1 GB where it peaked at 3.6 GB at 96 threads.
- Make a store index commit in a view bootstrap or an index build wait for its writes without holding a worker, so the peak memory of a bootstrap stays flat as its files grow.
- Run the whole process on one runtime: pipelines, directory scans, index builds, the store, the caches and Python all share it.
  `configure_default_runtime()`, Python `Runtime(threads=...)` and `dftu_runtime_new()` grow or shrink its threads in place while work runs, and only process exit stops it.
- Keep the rollup and lookup caches on the store, so several processes can write one cache folder.
- Spill the bloom chunks of a large file on the store into copies that no reader sees until the file's index is complete.
- Cut the peak memory of an index build on the store backend: the build keeps each key once until its commit and writes it into its extent without more copies, and page dictionaries train as tasks of the build's executor.
  A cold `dftracer_index` of the 8,404 minife traces at 96 threads peaks at about 2.8 GB where it peaked at 3.6 GB.
- Build each file of `distributed_index` on one worker, with that worker's threads, where a file larger than its share of the work was cut across workers.
  The `partition` argument keeps its values, and both place whole files.
- Let a process that holds an index on the store backend see the files other processes wrote to it within 1 s, without a reopen.
  `DFTRACER_UTILS_INDEX_STORE_REFRESH_MS` sets that interval.
- Build zstd 1.5.7 from source for every build, as one static archive with its symbols hidden, where a build found the system zstd first.
  A system zstd can be much older (1.4.4 on Corona), and a program that also loads the system libzstd no longer has its zstd calls bound to ours.
- Build the index store engine as its own library, `libdftracer_utils_store`, which links only the runtime libraries, libdeflate and zstd.
  The `dftracer_utils` target links it, so a program that links `dftracer_utils` needs no change.
- Bundle zlib-ng inside the utilities library with its symbols hidden, so a program that also loads the system zlib no longer has its zlib calls bound to ours.
  No public header includes a zlib header, and the gzip classes in `include/` hold their zlib-ng stream in a private part.
- Size how many stat batches a directory scan runs at once from the stat time it measures, where it ran at most 12 at once on any file system.
  The scan admits one more batch while each stat stays within 2 times the fastest it has seen and fewer when stats slow down, up to `--executor-threads`, and a cold view of the 8,404 minife traces on Lustre takes 9.1 to 9.6 s where it took 9.0 to 10.2 s.
- Stat the shared index folder once in a `dftracer_view` run, where each input file made up to 4 stat calls and 7 lstat calls on it.
  A view of the 8,404 minife traces makes 34,954 stat calls cold and 33,780 warm where it made 51,756 and 67,392, and 8,148 lstat calls warm where it made 66,913.
- Commit the index of a cold `dftracer_view` from each worker as it finishes its files, where one thread wrote all of them after the parse.
  A cold view of the 8,404 minife traces at 48 threads takes 7.5 to 8.6 s where it took 11.1 to 12.1 s, with the stored index unchanged.
- Stat the files of a directory scan on the pool of `dftracer_view`, `dftracer_index`, `dftracer_split` and `dftracer_run` instead of one at a time on the main thread.
  A cold `dftracer_view` of the 8,404 minife traces at 48 threads starts its parse after 0.3 s where it took 0.8 to 2.6 s and takes 11.6 s where it took 14.4 s, and `dftracer_index` takes 8.4 s where it took 8.7 s with the same index.
- Walk each record's keys once and its args once in the index fold's parse, where it walked the record twice and the args up to twice.
  A one-thread build of the 8,404 minife traces spends 138 s in the parse where it spent 166 s and takes 157 s where it took 185 s, with the stored index unchanged.
- Stop a worker that waits in `get()` for a task from spinning on the CPU.
  It now sleeps until that task finishes or new work comes, so on macOS a 200 ms wait costs under 2 ms of CPU where it cost 205 ms.
- Cut the CPU an index build spends per event by about 25% on minife traces at one thread: schema leaf paths are interned once per record shape and key, chunk statistics and dimension counts find their entries by interned id, a bloom insert hashes its value once and a value already in a chunk's bloom is not probed again. The stored index is unchanged.
- Bound the parse memory of each worker in an index build and a cold first query: a chunk is parsed in pieces of at most 1 MiB and 8192 lines, `parse_many` reads batches of 128 KiB on the calling thread, a record longer than a batch is parsed alone, and event buffers cut back what a huge batch or event grew when they are returned.
  The peak of a build no longer grows by about 100 MB for each extra worker.
- Admit the files of an index build and a cold first query by the bytes each holds under the memory budget instead of one per worker, let workers with no file parse pieces of files in flight, and give the strings of a file the same ids whatever the parse order.
  A build under any memory budget, a 1 MiB one included, stores the same index as an unbounded build: spill runs are never smaller than 4 MiB, so a tiny budget no longer writes thousands of small runs, and a cold first export commits its sink at its limit instead of after every file.
- Stat the entries of a folder in batches of 16 over the executor in the directory walk of `dftracer_index`.
  On the 8,404 minife traces at 48 threads the walk takes 0.53 s where it took 1.3 s.
- Rebuild the indexes and caches made before this change on first use.
  Every interface and format version of the library is now 0, and a stored index, view cache, viz summary or frame in another version is not read.
  A missing or unreadable manifest or format record also means a rebuild.

- Make the tools stat each input file once and open it once per pass over its bytes.
  The directory walk reads the size and mtime of a file in one stat, and the later steps reuse them where they used to stat it again.
  `dftracer_index` opens each file once and reads its hash, schema and gzip index from that open file, and `dftracer_view` and `dftracer_split` keep the descriptor of the first look at a file for the pass that follows.
  On a parallel file system this cuts the metadata calls per input (a Corona run of 500 files made 12 stat calls and 4 opens per file in `dftracer_index`, and 57 stat calls per file in `dftracer_stats`).
  `dftracer_stats` reads the files once to build the category and the pid and tid counts, where it read them twice.
- Keep one file in flight per worker in the first run of `dftracer_view` and any View over unindexed traces, with each worker writing its files into its own write of the index, where the run took files in waves of 16 and wrote each file's index under one lock.
  Each worker merges the aggregate states of its files as they finish, a sink commits once it holds 32 MiB of memory (it counted only keys and values before), and the export first run takes the same path with one open of each index root.
  A worker that fails stops the others.
- Index the trace files in subfolders of `-d` before `dftracer_event_count` counts, as the other tools do, where it indexed only the top folder up front and built the rest as it counted.
- Check whether `dftracer_split` input files lie in the output folder by their walked path, and the output folder by its canonical path, instead of resolving every input file.
  A symlink that points into the output folder through a different spelling is no longer skipped.
- Replay the input files of `dftracer_replay` in parallel, as many at once as `--executor-threads`.
  Each file keeps its own open files and its own timeline, so files of different processes or times no longer wait on the first file's clock.
  `--max-events` still caps the whole run.
- Split traces without building an index.
  `dftracer_split` reads each file once and writes chunk files of about `--chunk-size` as it goes, where it built an index of every file first and planned its chunks from it.
  Chunk files are named `<app>-<group>_chunk<k>.pfw.gz`, and they no longer start with a `[` line or end with a `]` line.
  A chunk file ends at the gzip member end nearest to `--chunk-size`, so its size is within about half a member of the request.
- Bound the memory of the `dftracer_split` chunk writers by `--memory-budget`.
  A group of files starts only while the budget holds its writer, and one group always runs.
  Each writer used to assume a third of the memory, and the tracked heap reached 705 GiB on a 68 GB set.
- Compress each gzip member into a buffer that is reused, and hand a full member to its compress job without a copy.
- Write the bloom tier of a small file through the shared index write, so many small files share one write.
  Only a file that spills while it is parsed writes runs of its own.
  On 1.09 GB of 77 KB traces (8,404 files) on Lustre, `dftracer_index` takes 189 s instead of 544 s with 96 threads and 84 s instead of 413 s with 16.
- Check the indexes of many files of one index root at once, so only a file that must build its index waits for the root.
  On the same set, `dftracer_event_count` takes 37 s instead of 101 s and `dftracer_info` takes 31 s instead of 91 s.
- Let only one idle executor worker poll on the short timer, so a quiet pool does not wake all of its workers.
- Read the trace files in all subfolders of `-d` in `dftracer_index`, `dftracer_event_count`, `dftracer_info`, `dftracer_stats`, `dftracer_split`, `dftracer_comparator` and `dftracer_run`, as `dftracer_view` and `dftracer_validate` already did.
  Folders that hold index data or split output are skipped.
- Read a gzip member larger than the checkpoint size through the restart points its index holds, in bounded memory, and write no copy of it.
  The line-range reader starts at a restart piece, and a read that starts at one drops the line it opens, which belongs to the piece before.
  The tools read a plain trace only after it is gzipped (`dftracer_pgzip`).
  `dftracer_index`, `dftracer_view` and `dftracer_run` no longer write a gzip copy under `split/`.
- Build the index of a set of files with every worker busy: a worker takes a block of consecutive file ids, writes each file into its block's write as it finishes and frees its state, so memory grows with the workers and not with the files.
  `dftracer_index` on the 8,404-file minife set takes 14 s instead of 60 s at 48 threads.
- Shrink the fixed cost of a string table from about 1.5 MB to about 120 KB, because its directories are now made as strings arrive.
  An index build makes one table per file, so a set of thousands of small files held several GB in empty tables.
- Rename the environment variables, CMake options and macros that began with `DFTRACER_` to begin with `DFTRACER_UTILS_`, so they do not clash with DFTracer.
  Set `DFTRACER_UTILS_CACHE_MAX_BYTES`, `DFTRACER_UTILS_DUQL_PATH`, `DFTRACER_UTILS_SCHEMA_PATH`, `DFTRACER_UTILS_INDEX_STAGE`, `DFTRACER_UTILS_INDEX_SCRATCH`, `DFTRACER_UTILS_INDEX_SIZE_FACTOR`, `DFTRACER_UTILS_JIT_CACHE`, `DFTRACER_UTILS_PLUGIN_INCLUDE` and `-DDFTRACER_UTILS_USE_ZLIB_NG`, and use `DFTRACER_UTILS_PLUGIN_FACTORY_SYMBOL` in plugins.
- Make each stateful lazy op (sort, top-k, group by, joins, `asof`, `interval`, window, `head_by`, `unique`, `is_duplicated`, pivot and compare agg) hold its state in a pool of its query and spill when the memory manager asks for bytes back.
  A second query that needs memory over the budget now makes the first one spill instead of going past the budget.
- Make `unique` and `is_duplicated` use up to half the peak memory, because their keys now sit in a chunked arena that never moves and their hash table grows four times at a time once it is large.
  On a 1 GiB budget, `unique` peaks at 163 MiB instead of 336 MiB on macOS and at 133 MiB instead of 163 MiB on Linux.
- Give memory back to the system at once when a run of 8 MiB or more is freed in the page heap, and purge cached pages before the heap maps past the budget.
- Give each `dftracer_genesis_gen_dist` group a memory pool with the limit of its share, so the memory of each group shows in `memory::pool_stats()`.
- Let an index build free its idle fold buffers when the memory manager asks for bytes back.
- Push `unique()` and `drop_duplicates()` into the trace scan, so a distinct row query dedups each worker's rows before they become frame rows.
  The result keeps the first occurrence in unit order, a key column the scan cannot key exactly falls back to the host cursor, and the host `unique` cursor now keys rows by a 64-bit hash with no per-row string allocation.
  On the 17 GB genesis sweep, `filter(gtype == "counter").select("run", "metric").unique()` takes 33.5 s instead of 592 s, uses 74 cores instead of 10 and peaks at 11.0 GB instead of 13.8 GB.
- Find the gzip members of a trace with 16 reads in flight instead of one, and keep the `sketch` strings of genesis records out of the index build.
  The member scan of the 20.7 GB genesis sweep took about 45 s on one core, and its sketches filled the string table to about 36 GB.
  The index of the regrouped 17 GB sweep now builds in 313 to 338 s at a 5 to 8 GB peak instead of 385 s at 42 GB ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Write the `run` and `func` records of `dftracer_genesis_gen_dist` after every `counter` record, in gzip members of their own, so a query on `func` or `run` records reads a few chunks instead of most of the file.
  On the 20.7 GB sweep, a `gtype == "func" and name == "main"` query takes 1.3 s instead of 44 s, and the amg scaling read takes 1 s instead of 13 s ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep the bloom index that a first `dftracer_view` query builds for each file within that file's share of `--memory-budget`.
  Past it, finished chunks go to a staged write of the index that is published with the rest of the file, as `dftracer_index` already did, and the index is the same ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep the min and max of each auto-indexed args key of an index chunk as numbers while the chunk is built, and format a value as text only when it moves a bound, instead of formatting every value and parsing both bounds again for each comparison.
  The stored bounds are the same text.
  The first `dftracer_view` count over one genesis part of 160 million counter records takes 397 s instead of 760 s on one node, at the same 12.4 GB peak ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Build the index of a gzip trace on all the cores it is given, in bounded memory, and give the same index.
  The members are inflated in runs of about 8 MB, which up to 16 workers take in order into reused buffers, with at most two runs past the workers ahead of the parse.
  The plaintext is parsed ahead of the index folds in 4 MB pieces, and every fold steps on its own task in file order.
  The bloom index of different chunks is built at the same time, each chunk from its records in file order.
  A first `dftracer_view` query over several files divides the cores between the files it indexes at once.
  Before, one file was inflated and its index built on about one core, and the workers ahead buffered up to 160 MB each without bound.
  Over the 12 parts (20.7 GB) of a genesis sweep on one 48-core node, the first `dftracer_view --group-by gtype --agg count` takes 222 s at a 36.9 GB peak instead of 863 s with 52 GB resident, and one part of 160 million counter records takes 172 s instead of 397 s ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Make `dftracer_genesis_gen_dist` sort and walk a run group in parallel.
  The calls of a group too large for its sort buffer are read back from the spill file, mapped and sorted by 16 tasks at once.
  The processes of a group are walked in up to 64 parts at once, process p in part p modulo the parts, and the partial records are merged in part order (exact sketch merges, counts, minima and maxima).
  A group of 10.3 billion calls from 64 processes takes 54 min on one 96-core node with a 56.6 GB peak, where the single-threaded walk alone ran at 130 K calls/s, about 22 h.
  Two small groups take 11 s instead of 1 min 44 s.
  The split depends on the group alone, so the output is the same for any thread count.
  The `sum` and `avg` of a group with several processes per set differ from the single-threaded walk in the last digits (at most 4e-12 relative on amg), and every other value is the same ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Make `dftracer_genesis_gen_dist` count the decoded gzip member that each open trace reader holds against the reader quarter of a run's share, sized from the file's last gzip trailer, instead of 8 MB per reader.
  A run of many files with large members now opens as many at once as its share holds, so the tool stays near `--memory-budget`.
  Four runs of 40 files with 20 MB members peak at 518 MB under a 512 MB budget ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Decode a gzip member into a buffer sized from the file's last gzip trailer, and grow both decode buffers without zero-filling them or copying bytes that are decoded again.
  Each line inside a member is now yielded in place instead of copied.
  This read path serves `dftracer_genesis_gen_dist` and every unindexed trace read.
  One 932 MB member reads in 0.92 s instead of 1.29 s with a third less peak memory and page faults, and a file of 16 MB members peaks at 113 MB instead of 147 MB ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep on PyPI only the newest `<tag>.postN.devX` prerelease of the previous release when a release is published, and delete the older ones, so the project stays under its PyPI storage limit.
- Move the shared helper headers from `dftracer/utils/core/common/` to `dftracer/utils/common/`, and `dftracer/utils/core/env.h` to `dftracer/utils/common/env.h`.
  Change your includes to the new paths.
  The namespaces did not change.
- Move `dftracer/utils/core/utilities/compose.h` to `dftracer/utils/core/compose.h`, and `dftracer/utils/core/utilities/monitor.h` to `dftracer/utils/core/coro/monitor.h`.
  Change your includes to the new paths.
- Add the library `dftracer_utils_common` for the shared helpers, with its own pkg-config and `find_package` names.
  The `core` library links it, and the `json` and `duql` libraries link it instead of `core`.
  Add `dftracer_utils_common` to your link line if you use only the helpers.
- Take the SONAME and the macOS compatibility version of each shared library from the `abi` line of `VERSION`, and its version from the `api` line.
  The file names change, for example `libdftracer_utils_core.so.0.0.1`, so link again and update any script that names a file.
  Linux shared libraries now link with `-z defs`, so a shared library with an undefined symbol fails at link time.
- Rename the library `dftracer_utils_core` to `dftracer_utils_runtime`, with its pkg-config and `find_package` names, and move its headers from `dftracer/utils/core/` to `dftracer/utils/runtime/`.
  The include-all header `dftracer/utils/core/core.h` is removed, so include the headers you use.
- Make `when_any` cancel its losers and wait for them to stop before it returns, so no loser runs after `when_any` returns.
  A loser stops at its next wait when it has a cancel token or a `cancel()`, and a loser with neither runs to its end.
  A `CoroTask` loser stops at its next wait with no exception, and one that never started does not run.
  `timeout()` gets a `cancel()`, so `when_any(op, timeout(5s))` returns when `op` does and no longer waits 5 s.
- Allocate coroutine frames (`CoroTask`, `Coro`, `AsyncGenerator` and the `when_all` wrapper) from the page heap and count them in the memory pool of their task.
  With `DFTRACER_UTILS_MEMORY_DEBUG=1` a destroyed frame is poisoned and kept out of reuse until its pool closes, so a late resume crashes on the poison.
  A scratch pool that closes while a frame made under it is still alive now reports a leak.
- Speed up small allocations in the page heap by 3 to 6% on macOS.
- Make a `CoroScope` that spawned children and is destroyed without `join()` abort the process in every build, where it used to log a warning and let its children run on.
  A scope now comes only from `run_coro_scope`, `with_scope`, `Runtime::scope()` or a spawn; an owner that joins the scope itself uses `CoroScope::long_lived()`.
- Make a `GzipLineWriter` or `PaddedStripedWriter` destroyed without `close()` on an executor thread hand its cleanup to a task instead of blocking that thread.
  Off an executor thread it still waits for the cleanup, and with no runtime left it leaks its state with a warning.
- Make executor shutdown wake every waiting coroutine with a cancelled error and wait until it ends before it frees the frame.
  This covers a semaphore, `AsyncMutex`, `Channel`, `AsyncOnce`, `SpawnFuture`, a timeout, `when_all`, `when_any`, a memory reservation and a read that waits for a peer.
  A memory reservation that shutdown cancels now reports `memory::Status::Cancelled` instead of `OverBudget`.
- Speed up repeated `duql` queries that use a lookup side.
  A query reads the lookup store read-only and opens it for writing only to store a side it did not find, so repeated queries reuse one open handle and no process holds the store's write lock between queries.
- Speed up trace scans by not zero-filling the read and decode buffers of each scan unit.
- Move `StringIntern` to `dftracer/utils/dstd/string_intern.h` in the `dftracer::utils::dstd` namespace.
  Include the new header and name `dstd::StringIntern`, because `common/string_intern.h` is gone.
- Make `StringIntern::insert_at_id` return `Ok`, `Conflict` or `OutOfRange` instead of throwing.
- Speed up `when_all` and `when_any` over a list of tasks, because tasks that end at once now share one wrapper coroutine.
  A `when_all` of 8 tasks that are already done now makes 9 frame allocations instead of 16.
- Serve the counters family of `collect_typed()` from the per-chunk column blocks of the index instead of the stored per-second system rows, which no build stores now, so an index build is smaller and faster.
  The counters frame keeps its rows and columns, and its rows now sort by host, name and time bucket.
- Compress index values larger than 1 MB, which the store wrote raw before.
  The index of a 69 GB trace set with every column kept drops from 3.3 GB to 1.4 GB.
- Require a unit such as `4MB` for the byte size flags that took a bare number of megabytes: `-s`/`--chunk-size` of `dftracer_split`, which defaulted to `4` meaning 4 MB, and `--read-batch-size` of `dftracer_index`.
  A bare number is refused with an error that names the flag, and the defaults are `4MB`.
- Vendor unordered_dense 4.4.0, tl::expected 1.1.0 and the patched moodycamel concurrentqueue in `include/dftracer/utils/dstd/vendor/` and include them from there, where `include/` included `<ankerl/unordered_dense.h>`, `<tl/expected.hpp>` and `<concurrentqueue.h>` from the CPM include dirs.
  A public header now reaches no CPM package, so a plugin built from a source checkout and a plugin built against an install prefix compile the same, and the vendored headers install with the rest of `include/`.
  `cmake/patches/concurrentqueue-thread-exit.patch` is removed: its change is in the vendored header.
- Add the ctest `public_third_party_headers`, which fails on an include under `include/` that is not ours, standard, system, `<mpi.h>` or vendored; simdjson and nanoarrow are its known exceptions until the API cut moves them out of `include/`.
- Parse YAML with the reader of `libdftracer_utils_yaml` in place of yaml-cpp, for record schema specs and the `--event-map` file of `dftracer_gen_dlio_config`, in the new library `libdftracer_utils_yaml`, which also holds a block-style YAML writer that the DLIO config uses.
  No yaml-cpp archive is in the link line or the install any more, and the same input is accepted, except that a mapping with a duplicate key is now an error, as YAML 1.2.2 requires.
  No source of ours except `common/error.cpp` builds with exceptions now.
- Replace `dftu.svc.writer` with `dftu.svc.fileio` (`abi/fileio.h`), the host's file readers and writers.
  The five writer calls (`merge_shards`, `writer_create`, `writer_open`, `writer_chunk`, `writer_close`) keep their meaning and move into the new table.
  It adds a line reader over plain and gzip files (`lines_open`, `lines_next`, `lines_close`), a chunk reader, a gzip line writer (`gz_open`, `gz_append`, `gz_close`), whole-file `compress` and `decompress`, and the file helpers `logical_path`, `mtime`, `size` and `sampled_hash`.
  The C++ SDK gets `Host::fileio()` with `LineReader`, `ChunkReader` and `GzWriter`, and the fileio library gets a plain-file line source, `async_streaming_plain_lines`.
  `dftu_task_run` now returns the task's status, so a `DFTU_END` task status reaches the caller.

### Fixed

- `group_by(...).agg(...)` over many groups holds about 60 bytes a group and aggregation during the call, where it held about 400: the partial states of a frame folded in lanes are merged and put in first-seen order in their compact form, and no longer expanded into one 128-byte field stat per group and aggregation. 262144 rows, 112656 groups and 15 sum aggregations take 124 MB of step peak memory instead of 677 MB and run 3.7 times faster; the results are bit for bit the same.
- `group_by(...).agg(...)` over 65537 to 262144 rows and about 10000 or more groups (estimated from the first 4096 rows), or over more rows and 2048 or more groups, folds each group in row order in groups that each worker owns alone, so no lane holds a table of the groups and nothing is merged: 262144 rows, 112656 groups and 15 sums take 40 to 42 MB of step peak memory instead of 114 MB and run 3 to 4 times faster. A float aggregate (sum, mean, var, std, skew, kurt, sumsq, prod, corr, covar, regr) now has the bits of the single-thread and the larger-frame result, which differ from the old lane order in the last digits. Frames with fewer groups still fold in lanes (started at once, and dropped when the estimate says many), which are sized from the first rows, merged one after another and released as they go, and put in first-seen order in place.
- Keep every digit of a double literal in a query built from an expression. The builder wrote it with six significant digits, so `Field("x") > 9007199254740992.0` read back as `9.0072e15`, and a filter on a float literal near or above 2^53 asked an index about another number and could skip chunks that hold matches.
- Keep the columns of an empty result. A `LazyFrame` that streams (a table, or `collect(batch_size)` on any source) and filters out every row returned a frame with no columns; `collect`, `collect_all` and `collect_async` now return the plan's typed columns with no rows when the plan can name them all. `stream` still yields no batch for an empty result; read the columns from `schema()`.
- Keep a runtime working when a thread cannot be created (the per-user process limit or the cgroup `pids.max`) instead of ending the process. A worker that cannot start leaves the pool at the workers it has, with one warning that names `DFTRACER_UTILS_THREADS`, and the pool tries again after a second; a blocked worker whose replacement could not start gets it on a later try. A runtime that starts only some of its workers runs with those; the timer, sysmon and I/O completion threads report an error, and the pool monitor and watchdog only warn. A Python batch UDF raises `RuntimeError` when its lane thread cannot start.
- Count the usable CPUs as Go, Java and Rust do: `common::hardware_concurrency()` is now the smallest of the machine's CPUs, the affinity mask and the cgroup CPU quota rounded up (v2 `cpu.max`, v1 `cpu.cfs_quota_us`), so a container with a quota no longer starts a runtime of every CPU of the host.
  The default compute and I/O thread counts also stay within the threads the cgroup `pids.max` has left; a count you set is kept as given.
- Free the copy of the schema that an Arrow IPC reader hands to its decoder; the decoder does not keep it, so each `Reader` leaked a small schema.
- Hold a store's directory open while the store is open, so a directory deleted and made again at the same path (which can get the same inode number) is not taken for the old store when no marker file is there.
- Load a JIT-built Python UDF library on Linux with the dataframe C ABI visible to it; it failed with `undefined symbol: dftu_series_null_count` because the extension loads that library with local symbols.
- Free the column handle under a cast that a JIT batch UDF builds for an unsigned column (one 16-byte leak per call).
- Let a Python thread that the interpreter ends at exit (a daemon thread inside a native call on Python 3.12) leave without `terminate called without an active exception`; the GIL is taken back in the call path, not in a destructor.
- Import `annotationlib` only on Python 3.14 and later, so `mypy` on earlier versions checks code that uses `dftracer.utils.jit`.
- Make a build that starts while another build writes the same index wait for it, so `Indexer.build()` after a dropped `build_async()` no longer fails with an empty `DFTUtilsValueError`. The store takes one builder per file, and the second build found the copy the first one had published. The errors of the store writer for a refused call (a closed writer, an unknown namespace, a copy that is not being built) now carry a message.
- Rebuild a changed trace file through `scan::from_files`, `View::from_files` and `TraceViewer` with the aggregation settings the index root was built with, so the rebuild no longer clears the aggregation tier of every other trace folder that shares the root. The root now stores every setting its params hash covers (the percentile switch, list and sketch accuracy, relative and normalized time, statistics, the interval, `group_by_file` and the extra group keys). A root with a config record that cannot be read back (the earlier layout) makes the rebuild of a changed file an error that names the root, so no other folder's tier is cleared; index that root again explicitly. A config write refuses an extra group key that is empty or holds `0x1F`.
- Give `groupby().head(n)` and `.tail(n)` with a negative `n` the pandas rows (all but the last or first `-n` of each group, with their labels and order) instead of no rows. The Polars `group_by` refuses a negative `n` with a `ValueError`, as Polars does.
- Keep the row labels of a labelled Series in `DataFrame({"x": s})`, as pandas: one Series or Series with equal labels give those labels, and Series with different labels are aligned on the union of their labels (a plain column next to them is a `ValueError`). A column named like its level keeps the name and the level becomes a hidden default label. The Polars names (`to_dicts`, `rows`, `iter_rows`, `row`, `width`, `schema`, `get_columns`) leave out hidden default labels; `to_arrow` and `to_pandas` still carry them.
- Rebuild a changed trace file through `scan::from_files`, `View::from_files` and `TraceViewer` with the aggregation settings the index root was built with (the interval, `group_by_file` and the extra group keys), so the rebuild no longer clears the aggregation tier of every other trace folder that shares the root. Settings the stored config cannot recover are an error that names the root.
- Keep an idle index writer from holding one extent buffer per namespace: the buffers are made on the first append and are not zero filled, so 64 retained index roots hold about 1 MB each, not about 14 MB, and 3000 new index paths in one process end near 240 MB of RSS where they ended near 1.6 GB.
- Open a retained index root under a shared lock and drop the one that went unused longest past the limit, so threads that open the same or other roots no longer queue on one mutex (8 threads on 32 roots: 540 thousand to 990 thousand opens a second).
- Take `groupby(...).head(n)`, `tail(n)` and `nth(k)` from the native `cumcount` of the key columns: 1M rows in 1000 groups take 12 ms where they took 190 ms.
- Read the index column of `sort`, `top_k` and `bottom_k` through its values when it is not a flat column (`to_index_vector` read the buffer of a selection or view as if it held the values).
- Raise `TimeoutError` when a Dask cluster has no worker, in place of waiting for ever, in `distributed_index`, the Dask viewer and `DaskFrame`. The wait for a first worker is `DFTRACER_UTILS_DASK_WORKER_TIMEOUT` seconds (default 60), and the error names the scheduler address.
- Make the group-wise `cumprod` of an `int64` or `uint64` column the exact integer product while it fits in 64 bits, converted once to `float64`; past that it goes on as a float. The running float product rounded at every step past 2**53.
- Key a NaN in a duql lookup as a group does: every NaN is one key, so `x -> t.col` and `x in (...)` find a NaN row, and `-0.0` equals `0.0`. A NaN used to match nothing.
- Answer `TraceViewer(...).collect_typed()` from the aggregation tier when the files asked for are some of the files of a shared index root, and when they live in several index roots with one aggregation interval.
  Before, the counters read required the query to name every file the index held, and a query on one trace dir of two that shared an `index_dir` fell back to a scan that returned raw event rows (or an error once the traces were gone).
  A `collect_typed()` that groups or aggregates and that the tier cannot answer, for example because the aggregation data of a file is stale, now fails with an error that names the reason, in place of returning raw event rows; a query with no group or aggregate still scans.

- Read the new index after one process deletes an index and builds it again at the same path.
  Before, `Indexer.rowset("files")` raised "the index holds no rows" and a typed read returned no rows, because the process kept the store of the deleted index open.
  `Store::is_current()` and `Store::generation()` (C++) tell a handle whether the directory on disk is the one it opened; `Store::open` and `Index::open` drop a handle that is not current, and the aggregation intern table is keyed by `Index::generation()`.
  A store directory holds one id in `store.id` (16 hex digits, made once with `O_EXCL` by the first process that opens the store with write access) and a marker file `id-<id>.mark`; no process removes them.
  A store is current while its marker exists, so the check does not depend on inode numbers, and a directory made again gets a new id.
  When only the marker is gone and `store.id` still holds the store's id, the marker is made again and the store stays current; it is replaced only when `store.id` is gone or holds another id, so a copy of the directory put over a deleted one, which keeps its id, is not detected.
  A failure of the check other than "not found" (ESTALE, EIO, EACCES, a timeout) leaves the store current.
  A short or invalid `store.id` is replaced with a temp file and a rename, and a reader waits for a file that a creator is still writing.
  A store made before this change gets its id from its first writer, and a reader without write access falls back to the directory's device and inode.
  A store that is no longer current refuses to open a writer or a snapshot, with a `NotFound` error, so it cannot write into the new directory.
  Opening an index that is already open costs one more `stat` call.

### Removed

- Remove `dftracer.utils.dfanalyzer`; it moves to the DFAnalyzer project. With it go `DaskTraceViewer.collect_typed_ipc_futures`, `dftracer.utils.arrow` (`ArrowTable`, `ipc_to_table`, `decode_dictionary_columns`) and `dftracer.utils.index.open_readonly_indexer`, which only it used.

### Added

- Add a second build of the string-match kernels and of the trace prefilter's per-line code with branch padding against the Intel JCC erratum, picked at run time on an affected CPU and set with `DFTRACER_UTILS_JCC_KERNELS` (`plain`, `jcc` or `auto`).
- Add `dstd::fmt`, which formats text with the format fixed at compile time (`dstd::fmt<"open {}: {}">(path, err)` or `dstd::fmt("open {}: {}"_f, path, err)`), in one allocation, and `dstd::fmt_to`, which writes into a caller buffer with no allocation.
  It takes `{}`, hex, zero-padded width and fixed float precision, and a wrong argument count or a bad spec does not compile.
- Add `dstd::fmt_join<"\t">(a, b, c)`, which formats its arguments with one separator between them, and `dstd::fmt_join_to`, which writes the same text into a caller buffer.
- Add `common::make_error<"...">(code, args...)`, which writes the formatted message straight into the error.
- Add `coro::fail_with`, which ends a task with a stored exception without a throw, for code built without exceptions.
- Add `coro::or_fail` and `CoroTask::or_fail()`, which give the value of a `dstd::Expected` or of a task that returns one, or end the calling task with its error, with no exception.
- Add `match`, `discard` and `or_fail` to `dstd::Expected`, and pass an error on with `return r.error();`.
- Add `trace::views::LiveView` and its Python adaptor `LiveTraceViewer`, which follow gzip traces that are still being written.
  `snapshot()` indexes the members written since the last call and returns a `View` or `TraceViewer` of every whole member, with no copy of the trace.
  An old snapshot keeps its rows, and `status()` reports each file as whole, tail or hole.
- Add `LiveTraceViewer.new_rows()` and `follow()`, which return only the rows that an earlier call did not return.
- Add a public key-value store API in `store/store.h`, a C ABI in `store/abi.h` and `dftracer.utils.store.Store` in Python.
- Add `Batch::erase` and `Batch::erase_range`, which apply in the same record as the puts of the batch.
- Add `Store::pack`, which merges small closed data files into one and deletes the files it replaced after a keep time.
- Make a `Store` handle that a fork child inherited fail each call with `ForkedHandle`, and join and close nothing when the child drops it.
- Add shared namespaces to the store: each scope holds the keys of every writer, where a plain namespace shows only the newest writer of a scope.
- Encode keys and values of the Python store with an order-preserving format by default, and decode them on read: `None`, `bool`, `int`, `float`, `str`, `bytes`, `tuple`, `list`, `dict` and classes that define `store_type_id`, `to_store` and `from_store` or use `register_type`.
  Pass `raw=True` to `Store.namespace` for bytes-like keys and values that C++ or the C ABI reads or writes. A namespace with a fold is raw.
  A namespace is generic in its key and value types, and `key_type` and `value_type` check them at run time.
- Add a public `IoBackend` with one `submit(IoOp&)` call, so a program can run its own I/O backend through `ExecutorConfig::io_backend_factory`.
- Add a public `FileSystem` that `runtime::fs` runs on, so `find`, `walk`, `stat`, `read_file` and `write_file` work on any store that `register_file_system` adds for a path scheme such as `s3://`.
  A `file://` path is local.
- Add `coro::CoroutineHooks`, which shows each enqueue, resume and suspend of a coroutine to your own callbacks, and `coro::CoroutineEventLog`, which records them for a reader on another thread.
  With nothing installed, an event costs one load and a branch.
- Add `coro::Executor::post_yield`, which queues a coroutine that gives up its slice behind the other work of the thread.
- Add coro stages and adaptors in `coro/stage.h`: the concepts `BatchStage` and `StreamStage`, the adaptors `then`, `recover`, `named`, `timeout`, `limit` and `maybe` that apply with `|`, `either` and `EitherStage`, and `erase` to `DynStage` and `DynStreamStage`.
  `co_await` on a `dstd::Either` of two `CoroTask` gives a `dstd::Either` of their values.
  On a stream, `timeout` bounds the wait for each item through `coro::with_item_timeout`.
- Add the library `libdftracer_utils_trace`: the index, the views and folds, the trace reader and the Arrow interchange.
  It links every library below it and none of `dftracer_utils_utilities`, which now links it. The `dftracer_utils` target links it, so a program that links `dftracer_utils` needs no change.
- Add the library `libdftracer_utils_io`: gzip readers and writers, the parallel writers, hashing, directory scanning, text filters and the file helpers (logical path, modification time, sampled file hash).
  It links `dftracer_utils_runtime` and `dftracer_utils_json`, and the `dftracer_utils` target links it, so a program that links `dftracer_utils` needs no change.
- Add `memory::Pool`, `memory::PoolHandle` and `memory::Reservation` in `dftracer/utils/memory/pool.h`: open a child pool of `memory::root_pool()` with a limit, `try_reserve` the bytes a job needs, back a `dstd::PoolVector` with the pool through `memory::PoolAllocator` and read its `live()` and `peak()`.
- Export `write_shard_set` and `consolidate_shard_set` from `dftracer.utils`.
- Add `dstd::File`, a file handle that reads and writes at explicit offsets only, with `read_exact_at`, `write_all_at`, `size`, `resize`, `sync` and a `close` that reports its error.
- Add `coro::sync_wait(executor, task)`, which runs a task on an executor and blocks the calling thread until it ends.
- Add `coro::run_on(executor, task)`, which runs a task on another executor, and `co_await coro::this_coro::executor`.
  A task now resumes its awaiter, and an I/O completion its waiter, on the awaiter's own executor.
- Add a public C++ key-value store API in `dftracer/utils/store/store.h`: open a directory, register namespaces, commit batches, publish several building copies in one atomic step and read snapshots with `get`, `multi_get` and scans.
- Add `json::Document` and `json::Value` in `dftracer/utils/json/value.h`, which read the fields of a JSON line on demand from one parse without simdjson in the header, and `duql::Query::matches`, which filters such an event with the same answer as the engine and reads only the fields the filter names.
- Store every number field of the records of each chunk as an exact column in the index, with the time, duration, pid, tid, phase, category, name and host of each record, so later queries read these values without parsing the trace.
  Indexes built before this change rebuild their aggregation data once.
- Build the index on the index store from `dftracer_index` and from `distributed_index`, with no staged files and no ingest.
  Each `distributed_index` worker writes the index directly, and on the 8,404 minife traces `dftracer_index` takes 12 s where it took 36 s on RocksDB.
- Add `DFTRACER_UTILS_IO_BACKEND` to force the I/O backend, such as the thread pool for a run traced with DFTracer's preload.
- Add `DFTRACER_UTILS_MEMORY_REPORT` to print the allocator's live, measured and limit bytes, the page heap's peak mapped bytes and its peak bytes in use at exit.
  The mapped peak counts address space, which reached 705 GiB on a split run that used 100 GB, so read `used_peak` for memory use.
- Add the library `dftracer_utils_memory`, the allocator that the other libraries will use, with its own pkg-config and `find_package` names.
  The `common` and `core` libraries link it.
  The CMake option `DFTRACER_UTILS_MEMORY_BACKEND` and the environment variable of the same name pick its backend, `page_heap` or `system`; the variable wins, and a value that is not one of the two stops the process at the first allocation.
  The sanitizer presets use `system`.
- Add the memory manager of `dftracer_utils_memory`, with pools that count the bytes of a query, reclaimers that give bytes back and a measure of the process memory.
  The variables `DFTRACER_UTILS_MEM_BUDGET`, `DFTRACER_UTILS_MEM_HARD_LIMIT`, `DFTRACER_UTILS_MEM_LIMIT_HEADROOM` and `DFTRACER_UTILS_MEM_SPILL_HEADROOM` set its limits and take bytes or a unit such as `4GB`.
- Add the debug mode `DFTRACER_UTILS_MEMORY_DEBUG=1`, which finds a write past the end of a block, a use after free and a double free in the heap, and `DFTRACER_UTILS_MEMORY_EXIT_CHECK=1`, which fails a test run that leaks.
  The page heap also reports to AddressSanitizer and Valgrind, so both tools see its blocks.
- Add the allocator classes `memory::Allocator`, `memory::PoolAllocator` and `memory::Arena` to `dftracer_utils_memory`.
  `memory::Allocator` charges the pool of the current `memory::Scope` and `memory::PoolAllocator` charges one pool that you name.
  `memory::Arena` hands out blocks from chunks in a pool of its own and frees them all at once on `reset()`.
  `memory::allocate` also takes a pool to charge, and `memory::reallocate` resizes a block in place when it fits.
- Add `memory::Block`, a reference-counted block with a 64-byte aligned payload, and `memory::BufferView`, a view of part of one.
  Dataframe columns now hold these views: their memory is charged to the pool of the current `memory::Scope`, and a slice, `head` or `tail` allocates nothing and runs 15% to 60% faster.
  Memory that Arrow or a C ABI caller hands in is not charged.
- `interval` joins run about 40% faster and use less memory when many intervals are open, because they no longer copy each right row on its own.
- Add the `dstd` containers on that allocator, such as `dstd::Vector`, `dstd::String`, `dstd::HashMap`, `dstd::Map`, `dstd::Deque`, `dstd::UniquePtr` and `dstd::SharedPtr`, and the types `dstd::Function`, `dstd::Any` and `dstd::RawBuffer`.
  A `dstd::HashMap` keyed by `dstd::String` finds a key from a `std::string_view` without a copy.
- Add the library `dftracer_utils_dstd`, between `memory` and `common`, with its own pkg-config and `find_package` names.
  It holds the hashing, eviction policy, `expected`, base64 and generic headers that the other libraries share, and it links only `memory`.
  Include paths change: the generic headers moved from `dftracer/utils/common/` and `dftracer/utils/core/` to `dftracer/utils/dstd/`, and `timer_service.h` moved to `dftracer/utils/core/`.
- Add `dstd::PoolVector`, `dstd::PoolHashMap` and `dstd::PoolHashSet`, which charge one pool that you name through `memory::PoolAllocator`.
- Charge a coroutine's memory to the pool that was current when it was made, on every thread it resumes on.
  `memory::PoolTag` and `memory::enter_pool` carry the pool, and a task made with no pool charges the root's general pool.
- Make `memory::Pool::reserve` ask the holders of the pool to give bytes back when a request does not fit, and return `Status::Pending` while they do.
  `memory::Pool::try_reserve` asks nobody and fails at once, for a holder that spills by itself.
- Add `reserve_async(pool, bytes, deadline)` in `dftracer/utils/coro/reserve_async.h`, which a coroutine awaits until its bytes fit or the deadline passes.
  Waiters get their bytes first in, first out, and a request larger than the pool limit or the budget fails at once with `Status::OverBudget`.
- Add `memory::Pool::raise_limit` to raise the limit of an open pool.
- Add `memory::ExternalSlot::add` with the kinds `Counted`, which counts toward the budget, and `Reported`, which only shows in `memory::Stats::external`.
- Show spilled bytes in `memory::Stats::external` under the slot `spill`.
  When the spill directory is on tmpfs, the spilled bytes also count toward the budget, and the process warns once with the directory and `DFTRACER_UTILS_SPILL_DIR`.
- Add the CMake option `DFTRACER_UTILS_SINGLE_LIBRARY`, off by default, to build one library `dftracer_utils` instead of one library for each area.
  The `dftracer_utils::shared` and `dftracer_utils::static` targets, the CLI tools and the Python extension link only that library.
  A build with the option on installs only `dftracer_utils`, with its own pkg-config and `find_package` names, and no per-area packages.
- Take the pandas names `col(...).isin`, `startswith`, `endswith`, `between`, `isnull`, `notnull`, `cumsum`, `cummax`, `cummin`, `cumprod`, `ffill`, `bfill` and `astype` in column expressions.
  `col(...).str` takes every pandas string method that the engine has, such as `startswith`, `lower`, `strip`, `len` and `split` ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Round to any number of places with `Series.round(decimals)` and `DataFrame.round(decimals)`, with halves to even as numpy and pandas do.
  A negative `decimals` rounds to the left of the decimal point, and an integer column stays integer ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Pivot on several keys and with any group-by reduction, such as `median`, in `DataFrame.pivot_table(values, index=[...], columns, aggfunc)` ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Take the keys of `TraceViewer.group_by` as one list as well as separate arguments ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `col(...).duplicated(keep=...)`, which marks the copies after the first (or before the last) as pandas does ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `str.isascii`, `str.encode` and `str.decode` for UTF-8 on a Series and on `col(...)`.
  `decode` raises on bytes that are not UTF-8, or gives null with `errors="null"` ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `Series.str.extractall`, which gives one row per regex match with the columns `row`, `match` and one per capture group ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `col(...).sketch_quantile(q)`, a group-by aggregate that merges the stored DDSketches of each group, such as `dur.sketch`, and reads the quantile `q`.
  It runs eager and lazy, and it gives what duql `quantile(merge(dur.sketch), q)` gives ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `str.normalize` (NFC, NFKC, NFD or NFKD, with utf8proc), `str.translate` (a table of codepoints to strings, integers or `None`) and `str.wrap` (`textwrap` of CPython 3.13.10, 3.14.1 and later, with the width in codepoints) on a Series and on `col(...)`.
  A row that is not valid UTF-8 gives null ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add the record schema setting `index.never_index`, a list of paths that the index build puts in the path catalog without evidence or interned values.
  Queries still read these paths.
  The built-in `genesis` schema lists `dur.sketch` and `v.sketch`, so the index build of a genesis file no longer interns the unique sketch of every function and counter record.
  A schema refuses a listed path that has a role or `always_index` or that a stored row set reads ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Add `--shard i/N` to `dftracer_genesis_gen_dist`, so that a process takes part `i` of `N` of the run groups.
  The groups are split largest first so the parts hold about the same trace bytes, and the parts concatenate into one file.
  A build with `DFTRACER_UTILS_ENABLE_MPI=ON` splits the groups between its ranks by default, and the ranks copy their parts into `-o` at their offsets at the same time ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Print the progress of `dftracer_genesis_gen_dist` to stderr.
  At the start it prints the run groups, their trace bytes, the shares and the member size.
  Then at most every 10 s it prints one line with the elapsed time, the groups done, how many groups wait for memory, read, sort, walk or write, the trace bytes read, the calls sorted and walked with their rates, the peak RSS against the budget and an estimate of the time left.
  At most every 60 s, each group that has run for a minute or more gets a line of its own with its step, its progress in that step and its time there ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Print a `DataFrame` or a `Series` the way pandas does, with the row index, right-aligned values and a common number of decimals for each float column.
  Past 60 rows it prints the first 5 and the last 5 rows with a `...` row and a `[N rows x M columns]` footer, and it reads only the rows that it shows ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Show a `DataFrame` as an HTML table in Jupyter, with the same rows and cells as its printed form ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).

### Removed

- Remove `ColumnExpr` and `Field` (use `Expr` and `col`), `TraceViewer.query` and `explain_query` (use `duql` and `explain_duql`), `TraceViewer.limit` and `DaskTraceViewer.limit` (use `head`) and `DaskTraceViewer.query` (use `duql`), and the native `Series.cast(type_id)`, which `cast(dtype)` replaces.
  `DataFrame.limit` and `LazyFrame.limit` stay as the polars alias of `head`, and `DataFrame.query` stays as the pandas name.
- Remove the native method names `Series.fillna`, `cumsum`, `cummax`, `cummin`, `nunique`, `ffill`, `bfill`, `skewness`, `_null_mask` and `_valid_mask`; they are the aliases `fillna`, `cumsum`, ... of `fill_null`, `cum_sum`, `cum_max`, `cum_min`, `n_unique`, `forward_fill`, `backward_fill`, `skew`, `is_null` and `is_not_null`, and `Series.is_null(i)` is `is_null_at(i)`.
- Remove `read_arrow_files_parallel`, `TraceReaderIterator` and `_MemoryViewBatch` from `dftracer_utils_ext`, and `_ArrowStreamingIterator` as a module attribute; `LazyFrame.stream` still yields frames.
  Read `.arrow` files with `pyarrow.ipc.open_file` and `DataFrame.from_arrow`.
- Remove `common::str_cat`. Build a message with `dstd::fmt<"...">(args)` and append text in a loop with `dstd::fmt_append<"...">(out, args)`, both in `dftracer/utils/dstd/fmt.h`: the format string is checked at compile time and the result is sized once.
- Remove `common::DFTUtilsException`, `common::throw_error`, `common::unwrap` and `duql::DuqlParseError`.
  Library calls return `dstd::Result`, so check the result where you called `unwrap` before.
- Remove `common::string_format`, `common::vstring_format` and the `DFTRACER_UTILS_PRINTF_FORMAT` macro. Use `dstd::fmt`.
- Remove `TaskExecutor::enqueue`. Use `post`.
- Remove the `--watchdog-idle-timeout` flag, which never had an effect.
- Remove `runtime/compose.h` with `utilities::op`, `AsyncOpFor` and the `&&` and `||` operators. Use the stages and adaptors of `coro/stage.h`, and `coro::when_all` and `coro::when_any` to run two things at once or to race them.
- Stop installing the C++ headers `runtime/io/io_backend.h`, `runtime/io/awaitable.h`, `utilities/filesystem/types.h`, `utilities/hash/*.h`, `common/input_stat.h`, `common/intern_table.h` and `dstd/hash/hash.h`, and remove `Indexer::open_scanned`. They are now private to the library.
- Remove the Python `JsonDictValue` class. No reader returned it.
- Remove the C++ `dlio::write_dlio_yaml(std::ostream&, ...)`. Use `render_dlio_yaml` and write the string to a file.
- Remove the `ghc::filesystem` dependency and `common/filesystem.h`. The library and the tests use `dstd::fs` and `dstd::Path`.
- Remove the C++ `IndexDatabase` class and its header `index/store/index_database.h`. Use `Index` from `index/index.h` instead.
  `index::release_root()` and `index::release_all_roots()` replace `release_index()` and `close_index_writers()`.
- Remove the `dftracer_utils_ext.IndexDatabase` class. Use `dftracer_utils_ext.Index` instead.
- Remove the Python `IndexDatabase.assign_file_ids` and `IndexDatabase.reserve_file_id_range` bindings, because a caller cannot use a file id that the process numbers for itself.
- Remove the file id from the indexer C++ API and C ABI, which drops `IndexedFile::file_id` and the `file_id` field of `dftu_indexer_file`.
  Find a file by its path instead.
- Remove `file_id_min` and `file_id_max` from the `shards.json` shard manifest, because nothing read them and file ids are no longer public.
- Remove RocksDB: the index store is the only index backend, and an index that RocksDB built is built again on first use.
  The environment variables `DFTRACER_UTILS_INDEX_BACKEND`, `DFTRACER_UTILS_ROCKSDB_CACHE` and `DFTRACER_UTILS_ROCKSDB_MAX_OPEN_FILES` and the CMake settings `DFTRACER_UTILS_ROCKSDB_VERSION` and `DFTRACER_UTILS_ROCKSDB_PREFIX` are gone.
- Remove `build_sst_batch`, `SstArtifactRegistry`, `move_artifacts`, `index_backend`, `IndexDatabase.bulk_ingest()` and `IndexDatabase.init_schema()` from the Python bindings and `resolve_local_staging` from `dftracer.utils.dask`.
  `build_index_batch` replaces `build_sst_batch`: it writes the index directly.
- Remove the `local_staging`, `shared_staging` and `flush_every_files` arguments of `distributed_index` and `build_index_distributed`, and `artifact_batches` from the result of `distributed_index`, since each worker writes the index directly.
- Remove `set_default_runtime()`, `try_install_default_runtime()` and `clear_default_runtime_if()` from C++, `dftracer.utils.set_default_runtime()` from Python and `dftu_set_default_runtime()` from the C ABI.
  Configure the one runtime with `configure_default_runtime()`, `Runtime(threads=...)` or `dftu_runtime_new()` instead.
- Remove the native `Runtime.shutdown()` from Python, so `Runtime.shutdown()` and leaving a `with` block release the handle and leave the runtime running.
- Remove `plan_work_units` and `enumerate_gzip_members` from the Python bindings, since `distributed_index` no longer cuts a file across workers.
- Remove the `DFTRACER_UTILS_ENABLE_ZSTD` and `DFTRACER_UTILS_ENABLE_LZ4` CMake options, and with them LZ4 compression of the index.
  Every build links zstd, so spill files and Arrow IPC files can always compress with zstd.
- Remove the `DFTRACER_UTILS_USE_ZLIB_NG` option and the fallback to madler/zlib.
- Remove the `-r` and `--recursive` options of `dftracer_replay`, since it now reads every subfolder of a directory input.
  Drop them from scripts that call it.
- Remove `ReplayEngine::add_executor` and `ReplayEngine::process_trace_line`.
  Each replayed file now gets its own POSIX or DFTracer executor.
- Remove the `--index-dir` and `--force` options of `dftracer_split`, since it builds no index.
  Drop them from scripts that call it.
  `--checkpoint-size` stays and sets the gzip member size.
- Remove `ChunkManifestMapperUtility`, `DFTracerChunkManifest` and `DFTracerChunkSpec`.
  `ChunkExtractorUtility` takes a list of gzip files and returns the chunk files it wrote.
- Remove `tracked_bytes.h`, its gauges and the CMake option `DFTRACER_UTILS_TRACKED_BYTES`.
  Read the bytes of each pool with `memory::pool_stats()` instead.
- Remove `MemoryPool` (`memory_pool.h`).
  Use a `memory::Pool` child with a limit and a `memory::Reservation` instead.
- Remove `track_default_args`, `custom_metric_fields` and the C ABI field `aggregation_track_default_args`.
  They fed per-argument stats that every index build stored and no query read.
- Remove `IndexDatabase.write_aggregation_tracker` and the process-parent tracking of the aggregation tier, which every index build stored and no query read.
- Remove `CoroScope::join_all()`, which was an alias of `CoroScope::join()`. Call `join()`.
- Remove the C++ `CoroScope`, `Task`, `TaskGraph` and `Pipeline` types. Use `coro::Scope`, `coro::with_scope`, `coro::parallel_map` and `coro::JoinSet`.
- Remove `DFTUtilsException::code()`. Read `condition()`, `error()`, `domain()` or `code_int()` instead, or `errorcode_of(e.error())` for the coarse code an `ErrorCode` names.
- Stop installing the C++ headers under `dftracer/utils/server/` and `dftracer/utils/utilities/`, except `utilities/filesystem/types.h`, `utilities/hash/types.h` and `utilities/hash/fnv1a_hasher_utility.h`, and the `utilities/utilities.h` umbrella. They are now private to the library.
- Stop installing the C++ `trace/trace.h` umbrella, the `trace/comparator/` headers, the `trace/event_collector_utility.h`, `trace/chunk_extractor_utility.h` and `trace/metadata_collector_utility.h` headers and `index/build/chunk_indexer.h`. `CompareView` is no longer a public C++ API; compare traces with `TraceViewer.compare` in Python.
- Stop installing the C++ `Pipeline`, `TaskGraph` and `Task` headers under `dftracer/utils/runtime/` (`pipeline/`, `task_graph/`, `tasks/` except `coro_scope.h`) and `runtime/abi.h`, the `coro/task_abi.h` header, the duql `errc.h`, `pattern.h`, `subsumption.h` and `internal/` headers and the trace statistics, provenance, `sharded_view.h`, `typed_rows.h`, `event_id.h`, `parse_inflated.h`, `chunk_verifier_utility.h` and `internal/` headers. `ShardedView` and the typed `rows` reader are no longer public C++ APIs; query shard sets with `dftracer_view` and use `TraceViewer.rows` in Python.
- Stop installing the C++ headers of the internals: `common/` (`archive_format.h`, `common.h`, `external_sort.h`, `field_ref.h`, `filesystem_info.h`, `format_detector.h`, `memory_budget.h`, `memory_config.h`, `scratch.h`, `sharded_mutex.h`, `spill_dir.h`, `spill_file.h`, `symbolize.h`, `test_probes.h`), `memory/` (`arena.h`, `asan.h`, `block.h`, `budget.h`), `coro/adaptive_gate.h` and `coro/reserve_async.h`, most of `index/` (the index, key, schema class, record schema, gzip, extension, plan, store and schema headers; `index/indexer.h` and `index/abi.h` stay), `dataframe/agg_spill.h`, `distinct_sketch.h`, `frame_ops.h`, `log2_histogram.h` and `timestamp_histogram.h`, `json/canonical.h`, `json.h`, `json_doc_guard.h`, `json_escape.h` and `record_parser.h`, `plugins/compose.h` and `plugins/plugins.h`, `runtime/io/io.h`, `io_sendfile.h` and `ops.h`, `runtime/distributed/`, `runtime/mpi/mpi_utils.h` and `store/store.h`. The typed `Op` compose layer for plugins, the typed `Index` API, the C++ record schema classes, `DistinctSketch` and `Log2Histogram` are no longer public C++ APIs; use `@jit.op` and `dftracer.utils.schemas` in Python.
- Stop installing the C++ duql `ast.h`, `ast_string.h`, `string_literal.h`, `evaluator.h`, `fields.h` and `parser.h`, the trace `event.h`, `schema.h` and `views/result_join.h`, and `time_metric.h`'s `extract_time_metric`. The AST moves to the `duql/detail/` headers, which are not public API, and `Query::from_node`, `Query::root()` and the `Expr` node accessors are no longer public. `DuqlError`, `DuqlParseError`, `Params`, `ValueMap` and `Cell` come from `duql/error.h`, `duql/types.h` and `duql/value_map.h`.
- Remove `duql::Query::evaluate(const json::JsonValue&)` and `trace::read_time_metric(path)` from the public C++ API. `Query::matches` filters a raw event.
- Remove the unused moodycamel readerwriterqueue dependency.
- Remove `dftracer/utils/json/json_value.h` and `dftracer/utils/json/parser.h` from the installed headers, and stop installing `simdjson.h`. No installed header includes simdjson now.

### Fixed

- Fix a segmentation fault when `argsort`, `top_k` or a frame `sort` read a column that is a selection, a view or a constant of numbers (a filtered Series in a frame): the kernel read the data pointer of a flat column. Such a column is materialized first, as a chunked one was.
- Fix `Series.cut` (and so a calendar-rule `resample` on a nanosecond index) to compare integer and timestamp values exactly when the breaks are integers too (float values or float breaks still compare as doubles, as pandas does): it read them as doubles, so a nanosecond value within about 256 ns of a break fell in the wrong bin. A sorted input now walks the breaks once. `resample` bins a week by integer arithmetic and `asfreq` returns at once when the index already is the grid; `at_time` and `between_time` compare at microseconds and read fractional seconds; `tz_localize` and `tz_convert` check the zone at the call and take the UTC spellings pandas does; `resample(offset="-3min")` parses and an `origin` whose zone differs from the index is a `ValueError`.
- Read the `config` field of a `@jit.plugin` class on Python 3.14. Annotations are lazy there (PEP 649), so a class has no `__annotations__` entry in its `__dict__`: the config fields were not seen, `self.config.<field>` failed with `unsupported in @jit.each_event: Attribute`, and a config field with an unsupported type did not raise `JitError`. The JIT front end now reads class annotations through `inspect.get_annotations`, with `annotationlib` forward references on 3.14.
- Keep the real value of a double in a zone bound. The bounds were text with six decimals, so a chunk of doubles near `1e-7` was stored as `0.000000` and a filter such as `args.lat > 1.5e-7` skipped chunks that held matching events.
- A keyless aggregation over no rows is one row (`DataFrame.sum()`, `mean()` and the like, `group_by()` with no key): `sum` is 0 and the other reductions are null.
- `Expr.str.find` is null where the needle is absent (Polars); `Series.str.find` keeps -1.
- The linear quantile of more than 262144 rows ranks a NaN of either sign above every number, and it selects the two ranks without a full sort for Int32, Float32 and other widths as well as Float64 and Int64.
- `Series.clip` and `Expr.clip` with one bound on an integer column saturate an infinite or out-of-range bound in the kernel; the cast of an infinity was undefined.
- A column with no valid value gives `None` for `min`, `max`, `mean`, `median`, `quantile`, `var` and `std` only after the type check, so a string column raises `TypeError` whether it is empty or not; the engine reports "no value" (a NULL scalar from C++ `Series::min` and `max`, NaN from `mean`, `variance`, `stddev` and `quantile`) and the Python binding turns it into `None` in one place.
  The C++ `variance` and `stddev` are NaN for no value and, in the sample form, for one value.
  The C scalar tags gain `DFTU_SCALAR_TAG_NULL`.
- `to_pandas` tests expect what it documents: `Int64` for an integer column with nulls and `category` for a dictionary column.
- `Series.exp`, `Series.log` and the expression forms follow IEEE for zero, negatives, infinities, NaN and overflow: `log` of a negative is NaN, `log(0)` is `-inf`, `exp` of a large value is `inf` and NaN stays NaN.
  A vector that held such a value was computed outside the range of the SIMD routine and returned garbage.
  `sign` of NaN is NaN, and `clip` leaves a NaN value as it is and ignores a NaN bound.
- `Series.is_duplicated` and `is_unique` count every NaN as one value and `-0.0` as `0.0`; a lone NaN no longer counts as a repeat.
- `Series.min`, `max`, `mean`, `median`, `std`, `var` and `quantile` of a column with no valid value give `None` (Polars gives null) where they gave 0 or NaN; `sum` stays 0.
- `Series.quantile` takes `interpolation` (`linear` by default, `nearest`, `lower`, `higher`, `midpoint`) and ranks a NaN value above every number, as Polars does.
  The C ABI gets `dftu_series_quantile_interp` and the C++ `Series::quantile(q, Interpolation)`; `DataFrame.quantile` passes `interpolation` through.
- `Series.str.contains` and `Expr.str.contains` search a regex by default, as pandas and Polars do; `regex=False` or `literal=True` keeps the substring kernel.
  `Series.str.replace_all` and `Expr.str.replace_all` take a regex by default, and `literal=True` the literal form; `Series.str.replace` takes `regex=` and `literal=`.
  `case=False` with a regex runs the pattern with `(?i)`.
- `DataFrame.drop(axis=...)` takes `None`, `1` and `"columns"` only; every other value raises `ValueError`.
- `Expr.clip` takes `lower_bound` and `upper_bound` (either may be `None`), the Polars names, in place of `lower` and `upper`.
- `DataFrame.top_k` takes `(k, *, by, reverse=False)` as Polars does; `nlargest` and `nsmallest` keep the pandas order.
- `Series.convert_dtypes` and `DataFrame.convert_dtypes` turn a Float64 column of whole numbers into Int64 (nulls kept), as pandas does with `convert_integer=True`.
  A float NaN is a value in the engine, so a column that holds one stays Float64.
- The parity gate is exact and strict.
  A findings line is one case (`kind.method#N[input]`) and must fail; a case that is not listed must pass.
  NaN and null are separate values, a bool never equals an int, a dtype is its Arrow spelling and a float must match to a relative 1e-9.
  When both libraries raise, the exception class must match.
- A forced index build (`Indexer.build(force=True)`, `dftracer_index --force`) of one trace folder no longer clears the aggregation of other folders that share its `index_dir`; only the forced files' aggregation rows are rebuilt.
- `group_by(...).agg(...)` keeps the type of a temporal column: `min`, `max`, `first` and `last` of a Timestamp (any unit and zone), Date32, Date64, Time32, Time64 or Duration give that type, the `sum` of a Duration is a Duration and the `mean` of a Timestamp or a Duration keeps its type, as pandas and Polars do; they gave an Int64 column that later `.dt` calls read as microseconds.
  The lazy plan `schema` after `group_by(...).agg(...)` and `group_by_dynamic` carries the same types, from one rule in `agg_output_type`.
- `group_by(...).agg(...)` with `first` or `last` gives null for a group with no present value, for every column type; it gave `0` or an empty string.
  The sum of such a group stays `0` and `count_valid` `0`, as Polars gives.
- The mean of an integer, Timestamp or Duration column in `group_by(...).agg(...)` divides the exact 128-bit sum; it divided a rounded double sum, which put a mean of nanosecond timestamps near 1.7e18 off by over a millisecond.
  A Timestamp or Duration mean is cut toward zero to its unit.
- `agg_merge` leaves two partials of another Timestamp, Date, Time or Duration type, unit or zone unmerged and holds the error for `agg_check`; `agg_deserialize` refuses a blob with trailing bytes or a time unit above nanoseconds; the on-disk rollup tag is `accumfmt3` and the distributed partial tag is 3, so a rollup or a partial of the old layout is recomputed or refused, not misread.
- A `group_by` that spills to disk keeps the unit and the zone of a Timestamp, Time or Duration key; they came back as microseconds with no zone.
- `group_by(keys).min()`, `max()`, `sum()` and `mean()` and the whole-frame forms include the Timestamp, Date, Time and Duration columns that the aggregate keeps in their own type (min and max of any of them, the sum of a Duration, the mean of a Timestamp or a Duration); they skipped them.
- `Series.rfloordiv`, `rmod`, `rpow`, `rdiv` and `rtruediv` with a Series operand compute `other <op> self`, as pandas does; `rdiv` and `rtruediv` raised `TypeError` for a Series operand. `s == None` is a mask (all False) and `s != None` all True, as pandas; a comparison with an operand that is not a number, a string, a bool or None returns `NotImplemented`, so the other operand answers.
- `dftracer_view` and `dftracer_genesis_gen_dist` under MPI no longer drop every rank to one thread when the launcher binds ranks to CPUs: a rank whose affinity mask is smaller than the node keeps its own CPUs, and only unbound ranks split the node.
- On Linux with GCC, a C++ plugin now unloads when the host closes it: the Python JIT, the test build and every module that links `dftracer_utils_dataframe` (the plugin templates, the examples and plugins built against the installed package) compile with `-fno-gnu-unique`, since GCC binds the statics of inline functions and templates as GNU unique symbols and `dlclose` then keeps the library mapped.
- The CLI tools print the right default and help text for an option whose text is built at start-up, such as `dftracer_server --checkpoint-size` and `--member-cache-size`; `--help` printed freed memory there.
- `dftracer_server` with `--index-dir` shared by several trace folders: a stale folder deleted the whole index store, which held the other folders' index data too. It now drops only its own files, including files removed from it, and a sub-folder counts as a folder of its own. A relative `--dir` now finds removed files.
- Fix a use after free when a plan ran an op after `dftu_op_unregister` removed it.
- Close a plugin channel once every plugin that declared a sending end of it has finished `on_finalize`, and not only when the scan ends, so a receiver whose finalize reads until `DFTU_PORT_CLOSED` no longer hangs when its sender never calls `channel_close`.
  A plan now keeps the op it uses, and unregistering removes only the name, so a later lookup by name finds nothing.
- Fix `to_pandas()` giving `int64` and `uint64` columns the numpy types `longlong` and `ulonglong`, so a pandas `merge` on a repeated `uint64` key raised `KeyError`.
  The columns are now numpy's own `int64` and `uint64`.
- Fix a read of freed memory when a task that waits in `with_timeout` or `when_any` on a timeout is cancelled, which a later cancel on the same executor could hit.
- Write every float of the DLIO config with a decimal point (`mean: 1.0`, `max_bound: 1.0`), where a whole number was written as `1`, so a YAML reader such as PyYAML reads it as a float and not as an int.
- Fix `dftu_task_when_all` and `dftu_task_when_any` ending the process when an input task raises an exception.
  The combined task now reports the exception.
- Fix `Store::commit` accepting a batch made before a `close()` when the next writer got the address of the closed one.
  Such a batch is now refused.
- Fix `dftracer_replay` reporting "No trace files" when it cannot read the trace directory.
  It now reports the read error.
- Fix a compressed Python export of an aggregated view reporting success when closing the gzip file failed.
- Fix a hang of a runtime with few threads when a task waits for other tasks while a blocked worker waits to run again, such as a duql join on one thread.
- Fix a line that spans two chunks being lost when a query prunes chunks.
- Make a reader stop at the verified end of the index, so it no longer reads bytes the index has not checked.
- Fix the parallel inflate of a gzip trace written by zlib on macOS, which ran on one worker because the member scan rejected its OS byte.
- Fix higher peak memory in `dftracer_index` and other trace tools: a thread now returns a freed block above 32 KiB to the thread that allocated it at once.
- Stop the false "cannot be read" warning when a store reader opens a data file that another process has just created.
- Fix a link failure for static `pkg-config` users of a Debug build, where the archive was named `libyaml-cppd.a`.
- Fix static linking from `pkg-config --static --libs` with pkg-config 0.29, which printed each library after the archives it needs.
- Fix store writes that fail with "operation canceled" on Linux with io_uring when the thread that started them exits.
- Fix a read blocked on a pipe or socket that stayed blocked through shutdown on Linux kernels older than 5.19 with io_uring.
- Fix a small memory leak when a task is cancelled before its I/O operation starts.
- Fix directory listings that blocked a worker on io_uring. They now run on an I/O thread.
- Fix `Runtime.progress` that showed a running task as queued, with no running tasks and no worker.
- Fix a data race in `coro::when_any` between a winner that cancels a loser and the start of that loser.
- Fix a false memory leak report and a failed exit check when a thread flushed its pool counts while a pool closed.
- Link a static consumer through pkg-config: each library's `.pc` file now requires every library of ours it needs and lists the third-party archives of its `Libs.private`, where `dftracer_utils_utilities` required only the runtime.
- Fix the row order of a `collect_all` or `TraceSession` plan that promises scan order (a duql pipeline with a stage after the scan, such as `select ... | where ...` or `derive`): it came back in worker order, and now reads on its own in scan order.
  Streaming row plans of one session no longer wait for a plan that cannot stream: they share one scan with a bounded buffer, where one such plan used to make every plan hold all of its events until the scan ended (3.3 GB and 1.5x the time for five row plans over 1.44M events, now 0.6 GB and 0.6x).
  A leading `select` of a plan that is not absorbed by the source streams too.
  Fix a hang when one plan of a streamed `collect_all` or session stopped early (`head(1)`) while a small memory budget held the others: the senders still waiting on its budget never woke.
  The cost: each ordered row plan in a batch now reads on its own, and a join or concat child reads apart from the streamed roots, so those cost one more scan than before.
- Fix `find_package` for the installed shared and static libraries: a project that links one now finds Threads, simdjson, nanoarrow, pcre2, highway, utf8proc, vectorscan, zlib-ng, zstd and libdeflate from the same prefix with no extra flags.
  The private static archives install as `libdftracer_utils_hs.a`, `libdftracer_utils_hwy.a`, `libdftracer_utils_hwy_contrib.a`, `libdftracer_utils_utf8proc_static.a` and `libdftracer_utils_zlib.a`, and the `zlib` package is now `dftracer_utils_zlib`.
- Read a line of any size: a line longer than the read buffer no longer fails the read of its file.
  The buffer doubles for that line and stays that size for the lines after it.
- Index every line the DOM parser refuses, with a number too large for a double, an invalid escape, a 20-digit integer, bad UTF-8, a raw control byte in a string or a nesting deeper than 1024 levels.
  The index build, the schema capture of a view scan or export, and `dftracer_event_count` read such a line on demand, so a query by equality on it finds it through the index.
  A line the DOM parser accepts is read once by the DOM parser, as before, and the index of such lines is unchanged.
  A line that cannot be read in full, such as a cropped one, is left out of the index and counted: `dropped_lines` in the build result of its file and one warning per file with the path and the count.
  `dftracer_event_count` counts only the lines the index build keeps and prints the same warning.
  A query literal with a `?` is no longer a required text for the line prefilter, since a reader writes `?` for a bad byte.
- Keep a line with a number too large for a double, such as `1e400`, and read that number as positive or negative infinity on the on-demand read paths.
- A listed path that holds a json field no longer gives a duplicate arg on the on-demand read paths.
- Keep a line with an invalid string escape, such as `\q` or a lone surrogate, and read that string as its raw text on the on-demand read paths.
  Event collection and containment in a batched session, by path or with nested captures, now read on demand and keep these lines.
  So do typed rows, the viz density scan, the plugin trace read, the event scan of a view, the fold scan of records decoded by path, and the indexed export of path records.
  Every other reader of trace lines that uses the DOM parser now reads such a line too: the aggregation query re-read, the viz folds (stats, summary, counters, process tree, density hashes and groups, events of a record schema), the provenance graph, the detailed statistics, the event collection, the trace config and time metric reads, the schema detection, the genesis processing, the reader query filter, `--select` and list columns.
  A line the DOM parser refuses is read on demand and written as a line it accepts: a number too large for a double becomes the largest finite double of its sign, an invalid escape stays as its raw text, a bad byte becomes `?`, and a part nested deeper than 128 levels is kept as a string holding its raw text. These DOM readers see a number past double range as the largest finite double, not infinity, and an integer of 20 or more digits as a double, not its exact digits.
  A line the DOM parser accepts takes the same path as before.
  The aggregation query counts a line that cannot be read in full in `dropped_lines` of its stats and warns once per file, and the event collection warns once per file.
  The density view no longer writes `"dur":inf` for an aggregate (`ph` 3) whose duration sum is too large for a double: the synthetic events carry no `dur`, and a `dftu_cnt` that is not finite counts as 1.
  A line nested deeper than the DOM limit of 1024 levels is now a valid line in the view scan, where it counted as invalid.
  The on-demand readers descend at most 128 containers (`MAX_READ_DEPTH`), so a very deep line never overflows the stack and is never dropped: a container below that depth stays under its path as its JSON text.
  A value nested between 128 and 1024 levels, which gave one value per leaf, now gives that text below level 128.
  Density keeps the first value of a repeated key in an event, as the DOM reader did.
- Fix the Python Arrow stream reading a column of another type under the first type: such a column now goes to `_extra` as JSON.
- Read an integer of 20 or more digits above the 64-bit range as its exact digits on the on-demand read paths.
- Read a line with bad UTF-8 or a raw control byte in a string, with `?` in place of each bad byte in the parsed fields; the raw line stays as it was.
- Add `TraceReader::dropped_line_count()` and one warning per read for lines that could not be read in full, such as a cropped line.
  A cropped line stays in `read_json` as text.
- Fix a crash when code read a field of a line whose document a read error had abandoned.
- Fix byte range reads that open more than 512 bytes into a line, which returned the tail of that line and lost the event.
  Adjacent byte ranges now give each line once and whole, at any cut and any line size.
- Keep the duration sum of a chunk at INT64_MAX where a larger sum overflowed and gave a negative sum or a wrong mean.
  The mean stays right until the statistics are reloaded from disk, where a saturated sum gives a lower bound.
- Recognize an integer `ph` in the metadata gate of a view fold, where only the letter form and unsigned integers past INT64_MAX were read before.
- Read each line of an indexed chunk as its own document, so a line with mismatched brackets, an unterminated string, a stray bracket or a length over 1 MiB can no longer crash the read, swallow the lines after it or end the chunk early; a line that holds two documents reads as its first, as on the plain path. The plain reader and the index build no longer drop a line over 1 MiB.
- Fix aggregation queries that aborted with "names chunk N past its N", or read the wrong chunks, after a file was indexed again with another chunk size.
  A re-index by the gzip indexer or the view's bootstrap build now drops the old per-chunk index data, such as the aggregation partials, and keeps the per-file catalog, profile and row sets.
- Make `Store::erase` return an `Unsupported` error when another data file holds the current copy of the scope, where it reported success and erased nothing.
- Close cache stores before the runtime stops at process exit, where a cache writer could wait on a stopped runtime.
  Every store, the runtime and the process caches now close from one `dstd::shutdown` that CLI tools and Python call while the runtime still runs, and the first hook registers it at exit.
- Fix index and store writes that could stall for minutes when many threads append to one store extent at once, because the appenders spun on a lock while the thread that held it waited to run.
- Fix a rare crash when a thread that queued work for a runtime exits while that runtime ends.
  The bundled concurrentqueue is updated and patched for this.
- Fix a crash when a runtime ends while a thread outside it, such as a store I/O thread, is still waking one of its tasks.
- Fix the `&&` operator of `runtime/compose.h`, which ran its two ops one after the other, so that both ops now run at the same time.
- Fix errors that were hidden: a view query now fails when an index cannot be read, when a stored zone map holds a bound that is not a number or when a `ts` bound is a string that is not an integer, where it used to read the file whole or skip the pruning.
  `dftracer_replay` now exits with 1 and names the token when a `--filter-pid`, `--exclude-pid`, `--filter-tid` or `--exclude-tid` list holds a token that is not an unsigned integer.
  A planner that has several chunks and no chunk table now fails, where it cut the file in equal parts that decoded the wrong bytes.
- Fix `dftracer_comparator` printing a Cohen's d that changed in its last digits from run to run, because the duration variance merged the files in the order their reads finished.
- Fix `dftracer_stats`, `dftracer_info`, `dftracer_event_count` and `dftracer_view` building a missing index on every hardware thread when `--executor-threads` asked for fewer.
  A cold `dftracer_stats` of the 2,376 amg traces at 96 threads on a 224-thread node takes 159 s where it took 199 s and peaks at 6.5 GB where it peaked at 10.8 GB.
- Fix an idle worker pool that shrank by one worker per keep-alive, which took about a minute on a 224-thread node; it now retires half of the workers above its floor each keep-alive.
- Fix a streaming scan that ignored its memory budget and a mid-scan `narrow()` on a host with more cores than the scan has chunks, because every chunk was claimed before the first one ran.
- Fix a scan budget that grew each time a chunk larger than the budget finished, which let later chunks run past the budget.
- Fix index answers that differed from a scan of the trace: records of a phase the View drops, such as instants, are left out of the index too, and a group by file keys a record without a file hash as null, not as an empty string.
- Fix an event with no `dur` reading as a duration of 0 on the scan, frame, index and `dftracer_stats` detail paths: a frame's `dur` column is null there and every `dur` aggregate (`count(dur)` included) skips the row, while `count()` still counts it.
- Fix a grouped view query whose filter reads a row set, such as `where fhash -> files.path == "/data/f10"`, failing with "'files' was read before its rows were bound" when the index holds aggregation data.
- Fix `dftracer_stats` parsing every trace again the first time it ran on an index: the index summaries could not give the largest event start, so the whole query fell back to a scan.
  The view now answers the largest start from the summaries, and logs at debug level why it scans when the summaries cannot answer a query.
- Fix `dftracer_comparator` reporting changes and regressions between two identical traces, and printing groups with equal changes in a different order from run to run.
  Means and totals now come from the exact integer sums and sum their values in a fixed order, so the same input gives the same output.
- Fix an index build that stalled for about 100 s on Lustre: it looked for the index's `schemas` folder once for every input file, from every worker at once, and one such lookup could block.
  It now looks once per index root, and a cold `dftracer_index` of the 14,117 minife traces at 16 threads no longer takes 122 s in 1 run of about 10.
- Fix `dftracer_split` writing no output on Lustre when an input gzip member is larger than a stripe; such a member now takes whole stripes of its own.
- Fix a pipeline that times out or stops ending the work of every other pipeline on the same executor.
- Fix a store index commit opening the whole store again to find sources it already held, which grew the peak memory of a view bootstrap with the number of files.
- Fix a data race between `Runtime.get_progress()` and an elastic pool retiring a worker.
- Fix a store index write from a worker of the default runtime hanging, as when a view builds its index.
- Fix a store index write that changes part of a file's data losing the rest of it when another process wrote that data.
- Fix `merge_shard_set` on the store backend, which kept no file of the merged index.
- Fix `distributed_index` leaving out the events of every part but the first of a file it cut across workers.
  A 4-member file built by 2 workers kept 1,205 of its 1,605 events, and it now keeps all of them.
- Fix a coroutine that waits on a timer hanging forever when the timer is set while the timer thread goes to sleep.
- Fix a file read or write waiting forever while the worker that started it runs a step that blocks.
  A worker now starts the I/O its step queued when the step ends, where it started it only after 16 queued operations or when it had no work.
- Fix every CLI tool crashing with a stack overflow when it runs under the DFTracer preload on Linux.
  The tracer's `munmap` wrapper allocated, which called the heap again before the heap's own `munmap` returned, and the heap now maps its memory with system calls that a wrapper does not see.
- Fix a second `dftracer_index` run on unchanged files indexing every file again.
  It now skips them, indexes a file again only when its size, its mtime or the index settings changed, and `--force` still indexes all.
- Fix `dftracer_index` exiting with status 0 when a file failed to index, and `dftracer_split` reporting 0 chunk files for a plain input.
  Both now print that the file is not gzip and exit with status 1.
- Fix a stack overflow crash of `dftracer_stats` and `dftracer_comparator` on thousands of files that share one index root.
- Fix the executor watchdog taking a busy executor for a deadlocked one when a long task runs with `--executor-threads 1`, which aborted `dftracer_split`.
- Fix the CLI tools dumping core on an error that leaves the tool, such as a watchdog timeout.
  They now print the error and exit with status 1.
- Fix `memory::Stats::measured` and `unattributed` counting other processes when the process runs in a cgroup, such as a batch job or a container.
  `measured` is now this process only, and the new `limit_used` holds the memory counted against `limit`, which the near-limit signal reads.
- Fix memory that was freed early in `dftracer_validate`, other tools and the index build when built with GCC 12.
  A lambda with a by-value capture inside a `co_await` was destroyed twice, and the `coro_lambdas` check now rejects it.
- Fix a data race in `when_all` over a list of tasks that return `bool`.
  Each result now has its own storage, so two tasks that end at the same time no longer write one byte.
- Fix index databases that were written with zlib, and aggregation rows that were written with no compression, in builds with zstd enabled.
- Fix a crash or a "RunLoop stalled" error when several threads each ran a plan or a scan with `get()` and shared an `AsyncMutex`.
  An unlock now resumes a waiter on its own executor, and a thread that drives a `RunLoop` waits for a waiter that another thread wakes.
- Fix a read of string offsets from an unaligned address when a spilled or cached frame was read back.
  A string or binary column read its offsets in place from the saved bytes, and the length of a column name could put them at any byte.
- Fix a crash with SIGILL when a build with GCC 12 read a gzip trace from the middle of the file.
  The compiler laid out the frame of the restart step in two ways, so a reused frame could stop the program.
- Fix the first query or build on a new index failing with `lock hold by current process` when a read-write open of the index ran while an older read-write handle of it was still closing.
  It happened on an index that a rebuild had reset while a reader still held it, such as the first `TraceViewer` read of 29 files with relative paths ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Match `_` and `%` literally in a `TraceViewer.filter` of `col(...).str.starts_with`, `ends_with` and `contains`.
  The filter escaped them without naming the escape character, so `starts_with("MPI_")` matched no record ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep the columns of an empty right side in a lazy join under a memory budget.
  A right plan that sent no rows failed with `join: no right column named <key>` ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fix duql `quantile(merge(x), q)` keeping the accuracy of the first sketch it read.
  A sketch of another accuracy than the default is now always skipped, so the result no longer depends on the order of the rows ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Reject a stored DDSketch with a bad accuracy or an out-of-range bin offset instead of reading it, which could loop forever in a merge ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fix `DataFrame.duplicated` marking every copy.
  It now marks only the copies after the first, as pandas does ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep the rows with a null key in `col(...).<agg>().over(key)`, eager and lazy.
  The result came back shorter than its frame, `cum_count().over(key)` failed, a lazy transform put values on the wrong rows after a null key and a lazy aggregate of a computed value was refused ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Return an empty frame from `GroupBy.median` and `GroupBy.quantile` of an empty frame instead of failing in the join of the quantile plan ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Leave the `run` records out of the first aggregate over a genesis file that has no index yet.
  The first-touch pass counted them, so `--group-by gtype` showed a `run` group that later queries did not ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fix a correlated duql sub-query on a trace with no index yet returning an empty frame when the outer filter could not be answered while the first-touch index was built, such as `where ph == "X" and dur > (from data | where name == ^.name | agg { m = mean(dur) })`.
  The side query reserved the file in the index root while the scan took that root for a built index and read zero bytes.
  The first build of a root now finishes before a scan reads the metadata of its files, and a root that does not hold the file is built ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Build the index that `dftracer_view` and `dftracer_index` need before their query or build on all their threads, not on the one thread that drives the command.
  The first `dftracer_view --query` on a genesis part of 197 MB takes 5 s instead of 31 s on a 48-core corona node, and a single file of 20.7 GB indexes in 584 s with 14 cores busy instead of 1 ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Print the chunks that a `dftracer_view` aggregation or `--query` read and skipped, which showed as `scanned=0 skipped=0`.
  A query for one run of a 20.7 GB genesis file reads 34 of 73,340 chunks ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Sort the result of `DataFrame.groupby` by the key columns, nulls last, as pandas does by default.
  `size`, `sum`, `agg` and the other aggregates came back in hash order.
  `sort=False` keeps that order, and `group_by` is not changed ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Index every gzip member of a trace that has zero bytes between two members, as the padded striped writer left before this release.
  The inflate worker stopped at the zeros, so the rest of its part of the file was missing from the index and from every count served from it.
  `dftracer_view` counted 9,547 of the 9,615 runs and 702,657,406 of the 710,692,194 counters of the 12 parts of an 8-node genesis sweep, and it now counts all of them.
  The zeros now belong to the compressed size of the member before them ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fail an index build when a visitor fails, for example when a line does not parse, instead of returning the part of the index built so far as if it were complete.
  The error now ends the inflate workers and reaches the caller ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fix `dftracer_view` dying with SIGILL (illegal instruction) on the first aggregate of a file when built with GCC 12.1 on x86_64.
  GCC compiled the call that builds the file's index on the way, awaited inside an `if` condition, into a trap.
  The awaited result is now named before it is tested, here and on the export path.
  GCC 12.3 on aarch64 and Clang were not affected ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fill every stripe of the padded striped writer with whole gzip members.
  When the space left after the data of a stripe was a little more than one full padding member, the last 1 to 24 bytes stayed zero, too few for another member, and `gzip`, `zcat` and zlib stopped reading the file there.
  Files written to striped Lustre, such as the output of `dftracer_genesis_gen_dist`, now read to their end with any gzip reader.
  Four of the 12 parts of an 8-node genesis sweep had such gaps, so `zcat` saw 146 of its 9,615 runs ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Make `dftracer_genesis_gen_dist` let a run group borrow memory of the budget that no group holds, 256 MB at a time, before it runs the group again with a larger share, and give the walk of a group the readers' and calls' parts of its share once its calls are sorted.
  On a full 8-node sweep each shard ran 74 to 154 groups again, and the last restart of the largest group threw away 25 minutes of reading and sorting, so 3 of 8 shards missed a 2 h limit ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Make `dftracer_genesis_gen_dist` run a group again with four times its share, up to the whole budget, when its counter statistics or path records do not fit, instead of skipping it.
  These grow with call paths times counters, not with trace bytes, so a 10 MB trace could need more than its 777 MB share, and a full run on 8 nodes skipped about 1,150 runs this way.
  The groups now share one memory pool, largest group first, and each group starts with its part of the pool by trace bytes, so one large group reads many files at once instead of one at a time.
  The tool now exits with 0 when it skips runs, and with 1 only when the output cannot be written ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep `dftracer_genesis_gen_dist` under `--memory-budget` on runs of thousands of trace files.
  Every file of every group opened at once to read the size of its decoded gzip member, so most opens failed with too many open files, and each such file was counted as an 8 MB reader while it decoded up to 120 MB.
  On 8 nodes this took the tool to 120 GB under an 80 GB budget, and one run of 6 groups to 20 GB under an 8 GB budget.
  At most 256 files are now opened for their size at once, and a file whose size cannot be read takes all reader slots of its group.
  The same run peaks at 4.1 GB and reads 10 times faster ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Keep `dftracer_genesis_gen_dist` under `--memory-budget` while it writes.
  Each run held all its output lines in one string and each worker held a whole 32 MB member, and neither was counted.
  The lines of a run are now made 1 MiB at a time from its records as they are written, and the group keeps its share until then.
  The member size is lowered until one member per worker fits the part of the budget for the writer.
  Under a 512 MB budget, 96 runs of 4000 call paths with 24 counters peak at 302 MB instead of 903 MB, in 29 s instead of 28 s, with the same lines ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Fix a hang at the end of every unordered `GzipLineWriter` that writes to a striped Lustre directory (stripe of 1 MB or more), such as `dftracer_genesis_gen_dist -o` on Lustre.
  The padded striped writer started its packer in the scope of the writer.
  The writer joins that scope before it closes the channel of the packer, and the packer ends only when that channel closes, so the join never returned and the output lost its last stripe.
  The packer now runs in a scope of its own ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Never spill to a RAM-backed directory.
  With `DFTRACER_UTILS_SPILL_DIR` unset and no writable node-local disk, the spill went to the system temp directory even when it was a tmpfs, so on a diskless compute node every spilled byte stayed in memory and `dftracer_genesis_gen_dist` was killed by the kernel at about 94 GB of 251 GB.
  The fallback is now the temp directory or `~/.cache/dftracer-utils/spill`, whichever is writable and not RAM-backed (a network file system qualifies).
  With neither, the first spill fails with an error that names the variable ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Make `dftracer_genesis_gen_dist` read runs that keep their time slices directly in the run directory (`nodes_<N>/ppn_<M>/<stem>-<k>_chunk<c>.pfw.gz`, without `compacted/`), which it skipped without a message.
  It also reads `raw/<set>/*.pfw.gz` for a set whose `compacted/<set>.pfw.gz` is missing instead of skipping the set ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Build the static zlib-ng archive as position-independent code, so a shared library links it when zlib-ng made no shared target (`BUILD_SHARED_LIBS` off) ([#112](https://github.com/llnl-asr/dftracer-utils/pull/112)).
- Grow an elastic runtime for a burst of tasks sent before its parked worker starts to run. The burst stayed on one worker for up to one `elastic_keepalive` (250 ms by default) before the pool grew.
- Fix a select-all frame that listed `args.*` columns of other traces.
  A plan declared the columns of every file in its index root, which holds all traces of one directory.
  It now declares only the columns of its own files.

### Fixed

- The memory exit check runs the `dstd` shutdown hooks before it counts, so a program that does not replace the global operator new, such as a benchmark, no longer reports its runtime, its stores and its query pools as live at exit and exits with status 70. The hooks were registered only with `atexit`, and the exit check registers its own handler at the first pool allocation, which can come later, so the check ran first. The first `dstd::add_shutdown_hook` call now registers `dstd::shutdown` as a memory exit hook as well. The `bench_smoke` ctests run with the exit check on, and the smoke tests of `channel_bench`, `fs_bench`, `pool_bench` and `io_bench` now carry the `bench_smoke` label, so `ctest -L bench_smoke` runs them.

- Keep a plugin library mapped while a column still borrows its buffer.
  `dftu_series_new_flat_borrowed`, `dftu_series_new_string_borrowed` and `dftu_series_new_string_view` keep the release callback's library loaded until the last column that uses that buffer is freed, so unloading a plugin set before its frames die no longer calls the callback in unmapped memory (one `dladdr` per borrowed column, not per row).

## [0.0.14] - 2026-10-10

### Changed

- Make the 2048-bin DDSketch of `dftracer_genesis_gen_dist` and the duql percentile folds grow its store with the key range it sees instead of holding all 2048 bins (8 KB) from the start: on 60 runs of 2000 call paths with 8 counters the tool peaks at 0.72 GB instead of 1.7 GB, runs that needed a 4 GB budget fit in 2 GB, and creating a sketch is 4.7 times faster. The sketch serializes to the same bytes. The genesis state pool now counts each growth of a store, and the 128-bin sketch of the aggregation engine keeps its inline array.
- Make `dftracer_genesis_gen_dist` find a call's counter samples from where the previous call of its lane left each series, instead of a binary search over the whole series for every call and counter: 4 runs with 64 counter keys take 3.9 s instead of 6.2 s, with the same output.
- Make `dftracer_genesis_gen_dist` write each run as soon as it is done, through its worker's own producer of a parallel gzip writer, instead of holding every finished run in memory until the runs before it in discovery order were written. One slow run early in the order no longer stalls the output and grows memory with every later run, and the writer now takes an eighth of `--memory-budget` instead of a budget of its own. The output holds the same lines as before, but runs, and the members of one run, appear in the order they finish.
- Run more registry frame ops in morsels under a memory budget instead of over the whole input: a `column_op` of `cumsum`, `cum_prod`, `cummax` or `cummin` over an integer column, or `ffill`, `diff` or `shift` (0 to 1024 rows) over a numeric column, carries the running value or the last rows from one morsel to the next, and its result is that of the op over the whole column (nulls and integer wrap-around included; a float sum, `rolling_*` and the other whole-column ops still run whole); `with_column` of a column takes the matching rows of it per morsel; a frame `filter` by a mask column runs as `filter_mask`, `sort_by_multi_per_col` as `sort_by_multi`, `group_by_dynamic` as the lazy op, and a frame `join` as the lazy `join` (a frame operand had never matched its lazy twin).
- Join a spilled partition that one key fills block by block for `lookup` and `nest` too, instead of holding it whole. A lookup keeps per key only the first right row and whether the rows of the key differ, then probes the left partition once, with the same errors as the in-memory join. A nest packs the keys into groups of a block and joins one group per pass, with the lists in right order as in memory; one key whose list passes the budget still makes a list that passes it. The per-key state of a lookup and the key totals of a nest are held in memory.
- Make a `group_by` read its input in row slices sized from the state bytes per row it has seen (1024 rows until the first batch shows them), so a state that grows fast per row, such as sixteen dyn names over nearly unique keys, is checked against the budget before one morsel has outgrown it: 1M rows hold 13 MiB above the streaming baseline at an 8 MiB budget on Linux instead of 97 MiB.
- Join a spilled partition that one key fills block by block: its right side is read in blocks within the budget and the left partition is probed against each, with the unmatched left rows of a left, outer or anti join and the matched ones of a semi join emitted once at the end, and a split that leaves every row in one part goes to the blocks at once instead of rewriting the right side at each level. Each probe of a spilled join takes as many left rows as the output share allows. One key on both sides, 4 left rows against 500,000 right rows, holds 30 MiB above the streaming baseline at an 8 MiB budget instead of 239 MiB. The untimed-events route of the viewer streams the sorted matches and keeps only the requested page, caching that page instead of the text of every match, so its memory is bounded by the page size however many records are untimed.
- Make the lazy `asof` and `interval` stream under a memory budget instead of collecting both plans: `LazyFrame::asof` and `LazyFrame::interval` (C ABI `dftu_lazyframe_asof` and `dftu_lazyframe_interval`, lazy ops `dftu.lazy.asof` and `dftu.lazy.interval`) sort both sides by (by keys, time) with the spilling sort and merge them, holding the right rows around the current left time (`asof`) or the ranges open at the current point (`interval`), and leave in morsels of a quarter of the budget. Rows, order, names and types are those of the eager ops (checked against them over groups, ties, nulls and NaN, unsorted input, all three directions, tolerances, outer on and off, and empty sides). At an 8 MiB budget over 1M left rows and 125,000 right rows they hold 20 MiB (`asof`) and 34 MiB (`interval`) above the streaming baseline. A plan with no budget, or whose columns are only known after it runs, still collects both sides. The lazy `asof` and `interval` check column names, key types and name clashes when the plan is built if both schemas are known, and a negative tolerance throws there.
- Pass the plan's memory budget to a plugin node: `dftu_node_vt::open` and the C++ `open` of `plugins/plugin/node.h` take a `memory_budget` in bytes (never 0), so a node that holds state can size its spill threshold and its output frames, as a built-in op does.
- Read and write spilled morsels with reused buffers (no new string and no zero-fill per morsel), and compress each spilled morsel with zstd level 1 when the spill file is on a network file system, where the bytes written cost more than the compression: on data shaped like the benchmark's it shrinks 2.6 times at about 1.7 GB/s. On a local disk it stays off, because it made `unique` 55% and `join` 90% slower and cost 15 to 20 MiB more memory there. `DFTRACER_UTILS_SPILL_COMPRESS=on` or `off` overrides the choice.
- Make a spilled `sort_by`, `sort_by_multi` and `reverse` merge their runs with a heap over the run heads and gather each output batch once, comparing typed key arrays instead of calling the column ABI for every cell: a `sort_by` over 4M rows at a 16 MiB budget takes 1.0 s instead of 5.2 s (10.5 s to 2.5 s on Linux), and `reverse` 0.6 s instead of 0.9 s. A spilled sort now also orders int64 and uint64 keys exactly: the merge compared every key as a double, so keys that differ only below 2^53 (nanosecond timestamps, for example) could come out in the wrong order. `is_duplicated` and `is_unique` stay in memory up to half the budget again (1.6 s instead of 4.2 s for 4M distinct rows at a 1 GB budget, as before the previous change), and so do `unique` and `head_by`.
- Add the group-key transforms (`dirname`, `basename`, `bucket`) of a View aggregation to each morsel as the scan streams, instead of collecting the whole scan into one frame first: on 1.5M events at a 4 MiB budget the aggregation holds 66 MiB less above the same aggregation without a transform. `TraceViewer.rows(cls)` now streams its rows in chunks instead of collecting them first, and the duql reference lists the stages (`pivot`, `bucket ... fill`, partition aggregates, the `overlap` lookup) that read one collected frame.
- Name each spill directory `<prefix>_<host>_<pid>_<n>` and remove only the directories of dead processes of the same host, so a spill root on a shared file system keeps the directories of other nodes. A run file whose morsel size or length is not in the file, a spilled group-by part past 4 GiB, and a write that makes no progress are now errors; `DFTRACER_UTILS_SPILL_COMPRESS` is read without regard to case, and `on` in a build without zstd logs a warning.
- Make `group_by(...).cummax()` and `cummin()` order a NaN as the greatest value and group float keys by value (negative zero and zero are one group, every NaN is one group), as the window plan always did. The whole-column `Series.cummax()` skips a NaN and is unchanged; the two rules differ.
- Make `join`, `explode`, `unpivot`, `to_dummies`, `take` and `group_by_dynamic` hand on their output in morsels that fit a quarter of the memory budget, and let a cross join spool a right side larger than a quarter of the budget instead of holding it. `to_dummies` also spools a quarter of the budget instead of all of it and no longer builds a morsel as wide as its distinct values times the whole morsel; at an 8 MiB budget it holds 14 MiB above its baseline instead of 27 MiB. A registry frame op no longer writes its input to a spool before it reads it all back.
- Run a registry frame op that has a native lazy op of the same name (`unique`, `sort_by`, `group_by`, `join`, `topk` and others) as that op, so it follows the memory budget, and run a `column_op` whose op is elementwise one morsel at a time. A spilled `group_by` with dyn aggregates now merges and finalizes one spill part at a time under the column names of all parts, instead of holding every group in one state. A registry frame op maps the part of its input past a quarter of the budget from a spill file, a plan with an op that can expand its input (a join, an explode, a registry frame op, a `group_by_dynamic` with overlapping windows and others) no longer takes the whole-column path on a resident source, and a `topk` whose `k` rows, at the width of the first morsel, do not fit half of the budget is cut from the spilling sort. A resident frame larger than half of the automatic budget streams under it too. `group_by_dynamic` hands on each window once a time past its end has been seen and holds only the open windows: half a million sliding windows over 1M rows hold 6 MiB above the streaming baseline at an 8 MiB budget instead of 79 MiB. A time that goes back now raises `std::invalid_argument` (`ValueError` in Python) in the lazy and the eager op; before, such a row went into a wrong window or was dropped. A sort of an input that sent only rowless morsels, such as a group-by of no rows, now hands on its typed columns instead of no morsel.
- Run `cumsum`, `cummax`, `cummin`, `cumprod`, `cumcount`, `shift` (non-negative), `diff` and `ffill` of a group-by as one native plan step over numeric columns, instead of a window, a sort by row number and a projection. With no memory budget, or a small input whose groups fit a quarter of the budget, it is one hash pass in input order and no sort; otherwise the input is spooled, sorted by (keys, row), streamed through a pass that keeps one group's state, and sorted back by row, so memory stays near the budget whatever the number of groups. Results are those of the window plan (null handling, output types, overflow errors), checked against the window plan itself over every integer and float width with nulls, NaN, infinities and negative zero, with and without keys, and with no budget and a small one. A float key groups by value (negative zero and zero are one group, every NaN is one group), and NaN is the greatest value for `cummax` and `cummin`, as in the window kernels. A single numeric key is grouped on its 64-bit value with no key string. It is 12 to 26 times faster than the window plan on 2M rows and 20,000 groups (0.03 s against 0.44 s for `cumsum`, `cummax`, `shift`, `diff` and `ffill`, 0.016 s for `cumcount`, in memory, on one Mac; run `group_transform_bench` to measure it).
- Make the lazy window stream under a budget: a chunk of the sorted input may end inside a partition, so a partition, or an input with no partition columns, larger than the budget no longer has to fit in memory. This covers running sum, product, min, max and forward fill, `row_number`, `rank`, `dense_rank`, `lag`, `lead`, `delta` and `rate`, row frames and range frames of min, max, count, integer sum, quantile, distinct count and arg min and max, float sums and means of row frames, and `sessionize`; and, from per-partition figures collected while the sort reads its input, `ntile`, `percent_rank`, `cume_dist` and frames over a whole partition (count, min, max, integer sum). It therefore covers `cumsum`, `cummax`, `cummin`, `cumprod`, `cumcount`, `shift`, `diff`, `ffill`, `bfill`, `rolling_*`, `rank`, `head`, `tail` and `nth` of a group-by. Results are identical to the window over the whole input (checked for every function above, with one, many and no partitions). Running functions are given a seed row that holds their value so far, counts and ranks are shifted by the rows already emitted, and lag, lead, delta, rate and the frames are given the rows around the cut. Variance and standard deviation frames, frames from the partition start (count, integer sum, min, max, float sum, mean and variance over a row frame), float sums and means over a range frame, collect frames and `first_value`, `nth_value` and `last_value` stream the same way, from state carried across the cut that repeats the kernel's floating-point steps; `last_value`, the shift of a variance and a window with more partitions than the figures table holds (an eighth of the budget) take their figures from one pass over the sorted stream, which is spooled (a quarter of the budget in memory, the rest on disk) and read back in step. A single partition over a million rows at an 8 MiB budget now peaks at 28 MiB above the baseline instead of 174 MiB. Frames to the partition end start from the partition's figures and drop the rows behind them: count, sum and mean over a row or a range frame (the rows with a missing order value get a frame of their own, and its float sum is replayed in a second pass over the spool, so the bits are the kernel's), and min, max, variance and standard deviation over a row frame (the sorted spool is also read backwards once, for the best of the rows still ahead and for whether the rows left are all equal). The sum, mean, variance and standard deviation over a whole partition come from the figures (a variance in a second pass over the spool). A text min or max from the partition start carries its best value across the cut, also when its frame reads rows ahead. A window may mix range frames with row frames. Still held whole, one partition at a time: quantile, distinct count, arg min, arg max and collect frames from the partition start or to the partition end, min, max, variance and order-statistic frames over a range frame that ends at the partition end, float sums, means and variance over a range frame from the partition start, and running functions mixed with ranks or look-around or sharing a column; `nth_value` holds back `n - 1` rows.
- Sort by integer, Bool, float and string keys, with any number of keys, through one stable radix argsort per key (last key first) instead of a comparison sort over every key: `sort_by_multi` on 4M rows and two Int64 keys takes 0.85 s instead of 1.9 s in memory and 1.1 s instead of 1.9 s at a 16 MiB budget, and a string key is ranked once instead of compared byte by byte. A float key now has one valid order in the sort kernels: every NaN is one value above infinity and `-0.0` equals `0.0`, so ties keep input order (before, `sort_by_multi` over floats with NaN compared NaN equal to everything, which is no order). `unique` and the scalar compare keep their order.
- Bound the memory of a spilled sort: one that wrote more runs than it can read at once now merges groups of runs into longer ones first (the fan-in follows the budget and the run morsel size, runs are written in morsels of about 128 KiB), so its memory no longer grows with the input: a group-wise transform over 16M rows at a 16 MiB budget peaks at 96 MB, where the single-pass merge reached 543 MB.
- Plan the lazy `compare_agg` as a rename, an outer join, a sort and two `with_column` steps, so it spills with the join and the sort instead of collecting both plans: 4M-row inputs with 2.5M groups each peak at 88 MB at a 16 MiB budget instead of 1.2 GB. When both plans know their columns and types the names and types of `l_`, `r_`, `delta_` and `pct_` are in `schema()` and a key mismatch throws when the plan is built; otherwise both sides are spooled, their names and types are read off the spools, and the same plan runs over them. The right plan of a `join` now runs under the budget of the plan that joins it when it has none of its own.
- Make the lazy `pivot` spill: it spools the (index, on, value) cells, aggregates them with the spilling group-by, sorts by the index with the spilling sort and widens the sorted cells a bounded batch of index values at a time, so memory stays near the budget instead of holding the input and the whole output. On 4M rows with 2.5M index values and 64 `on` values at a 16 MiB budget the process peaks at 76 MB instead of 3.5 GB; the output (index values ascending, columns by ascending `on` value, nulls where a cell is absent) is the same as before.
- Make a spilled `group_by` about 3.4 times faster at a 16 MiB budget on 4M rows with 2.5M groups (1.75 s instead of 6.0 s, peak 56 MB; in memory it takes 1.0 s). A hash part is merged first and split again only when its merged state really exceeds the budget (it was split by a size estimate, which rewrote about 1 GB in 340,000 pieces of 3 KB), a split folds its blobs into one state before it writes, pieces are cut from the state without re-hashing their groups, and spill blobs write each field statistic as a bit mask plus its nonzero words (`agg_serialize(state, true)`); the blobs the rollup store keeps are unchanged.
- Write View exports, indexed exports and aggregate exports through `GzipLineWriter`, deleting their own write loops. The indexed export now compresses and parses members in parallel (each worker folds its members into its own bloom slice): exporting the 2M-event bench trace with an index takes 0.68 s instead of 3.1 s. View export keeps its unordered parallel writes for parallel file systems through the writer's unordered mode and per-worker producers, at the same speed (176 ms); the aggregate export gives the same output at the same speed. The writer holds at most the memory budget (a third of available memory by default) ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Write the Python export sink, the file compressor (`dftu.file.compress`), the rechunker and the plugin trace writer through `GzipLineWriter`, deleting their own compress and write loops. Their outputs decode to the same bytes; members now end at line ends after the member size (the file compressor cut at raw byte counts, the rechunker started members with a newline), compression runs in parallel, and a file without a final newline keeps its last bytes. The plugin host writer ABI is unchanged ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Write the output of `dftracer_split`, `dftracer_pgzip`, `dftracer_genesis_gen_dist` and the fake trace writer through `GzipLineWriter`, deleting their own write loops. Every file decodes to the same bytes as before; members now end at line ends after the member size, so `dftracer_genesis_gen_dist` no longer writes a whole run as one member (new `--member-size`, default the checkpoint size) and `dftracer_split` no longer leaves an empty last chunk file. `GzipLineWriter` gains a final tail without a newline at close, caller-named parts with a header and footer member each, and a report per completed part. On laghos `dftracer_split` takes 4.5 s instead of 5.5 s; `dftracer_pgzip` is unchanged at 0.24 s ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Build a trace's path catalog on demand: `View.columns`, `column_info`, `schema_tree`, Python `TraceViewer.columns` and a duql wildcard path on a trace without a catalog first scan the files that lack one, for the catalog only, instead of listing only the declared fields or refusing the wildcard. This costs one scan once (about 0.25 s for the 53 MB laghos trace and for the 200-counter trace); later calls read the catalog ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Make the first full scan of a trace without a path catalog capture the catalog while it flattens `args`, so it walks each record once instead of twice and interns each path once per worker. On the trace with 200 counter paths the first `sort ts | take 1000` takes 2.33 s instead of 2.80 s (2.13 s once the catalog exists) ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Make the first query on an unindexed trace build its member table at full parallelism: with nothing reading the bytes, the index pass no longer copies every member to its dispatcher, and its channels no longer make workers wait for the ones before them. The index pass for the 53 MB laghos trace (158 members) takes 89 ms instead of 225 ms, so the first collect takes about 350 ms instead of 490 ms (a later one about 225 ms), and 700 ms instead of 790 ms for the same trace in one gzip member ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Make a trace written as one gzip member (plain `gzip`, Python `gzip`) scan in parallel: the index build now splits a member larger than the checkpoint size into pieces at restart points (bit offset and 32 KiB inflate window, the zlib `zran` method), and each piece is its own scan and pruning unit, read with zlib-ng from its restart point while normal members keep libdeflate. On the 53 MB laghos trace in one member a later collect takes 0.30 s instead of 1.8 s and the first collect (index build included) 0.78 s instead of 2.7 s; multi-member traces are unchanged. Line-range reads start at a whole member or the first piece of a split one ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Make reading a trace written as one gzip member (plain `gzip`) linear in its size: the reader, the index build and the member line reader decoded the member again from its start after every 1 MiB they read. On the 53 MB laghos trace in one member the first collect (which builds the index) takes 2.7 s instead of 9.3 s and a later collect 1.8 s instead of 5.1 s; multi-member traces are unchanged ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Make `to_polars` read the native Arrow stream through polars' Arrow import (polars 1.3 or later; no pyarrow needed), make `to_pandas` build a string column from its Arrow array when pandas keeps strings in Arrow (pandas 3 with pyarrow), and make `to_list`, `to_numpy` and pandas string columns decode each dictionary value once instead of once per row. The 1.3M-row laghos genesis frame converts to pandas in 131 ms instead of 657 ms and to polars in 143 ms instead of 715 ms, and `to_list` of a dictionary string column takes 5.4 ms instead of 32 ms; dtypes and values are unchanged. Float16, decimal and dictionary columns keep the native polars conversion, which gives them a different dtype than Arrow does ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `unique`, `nunique` and `value_counts` of a non-float column, `is_in` of a string column and a frame `unique` key rows by value ids built in place (a dictionary once per entry) instead of copying the column flat and building a string per row. On the 1.3M-row laghos genesis frame `value_counts` of `metric` takes 1.7 ms instead of 168 ms, a frame `unique` over `run, metric` 5.5 ms instead of 136 ms, `unique` of `path` 1.3 ms instead of 40 ms and `is_in` 1.4 ms instead of 27 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make column expressions (`with_column`, `filter`, duql `derive`, Python expressions) read their inputs in place instead of copying each one into a flat buffer first, and evaluate a collected (chunked) column chunk by chunk, keeping the result chunked. On the 1.3M-row laghos genesis frame a two-column arithmetic expression takes 1.9 ms instead of 5.4 ms, a string length 0.5 ms instead of 6.3 ms, and a lazy filter over the collected frame that keeps no rows 0.6 ms instead of 82 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `collect` follow the memory budget: once the parts it holds pass the budget (auto by default, a third of available memory), each further part is written to a spill file and read back through a read-only memory map, so the result stays the same frame while the operating system decides which pages stay in memory. Collecting an 8x copy of the laghos genesis trace with a 256 MiB budget peaks at 0.9 GiB instead of 3.3 GiB and takes 2.5 s instead of 1.4 s; the scan's string table is held in memory and is not counted against the budget. The spill file is removed from its directory when it is created, so nothing is left behind ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `collect` of a streamed query return chunked columns (the morsels it read, kept as chunks) instead of copying every column into one buffer, which cuts its peak memory by a third to a half: collecting the 1.3M-row laghos genesis trace peaks at 766 MB instead of 1097 MB and an 8x copy of it at 3.3 GB instead of 6.8 GB, and takes 213 ms instead of 228 ms. Every operation gives the same result on a chunked column; filters and string tests run per chunk, and group-by, join and sort join only the columns they read. `Series.encoding` is 5 for a chunked column. In C++, `Series::data<T>()`, `values<T>()` and `offsets()` give null or empty for a chunked column, as for any non-flat one, so code that reads raw buffers of a collect result calls `materialize()` first; `dftu_series_data` and `dftu_series_offsets` return NULL for it and `dftu_series_materialize` joins it. Plugins receive joined columns. A DataFrame's Arrow stream gives one record batch per chunk without a copy when every column is chunked the same way into flat chunks ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `DataFrame.slice`, `head`, `tail`, `Series.slice` and the row ranges that lazy queries read from an in-memory frame share their columns' buffers instead of copying the rows, for numeric, boolean, string, binary, view, dictionary and selection columns (a list, map or struct column is still copied), and make column expressions slice their string inputs the same way. Slicing the 1.3M-row laghos genesis frame takes 10 ms instead of 66 ms. `Series.slice` and `dftu_series_slice` now take string, binary, view and dictionary columns, where they failed ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make a View scan build each batch's rows from the batch's events without copying them when the filter keeps every event, write validity bitmaps directly and hand numeric columns their buffers without a copy, which takes collecting the laghos genesis trace from 219 to 202 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make a streamed collect build the columns of its final frame in parallel on its runtime instead of one after another, which takes collecting the 1.3M-record laghos genesis trace from 308 to 232 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make a View scan decode records 2.7 times faster on string-heavy traces: the string table takes inserts from many workers without one global lock and never rehashes a string as it grows, a string value is parsed as a number only for a field whose type needs one, and each worker reuses the previous record's sizes and field lookups. Collecting the 1.3M-record laghos genesis trace takes 289 ms instead of 770 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make a sort by one string key whose values repeat (at most a quarter as many distinct values as rows) rank the distinct values once and place rows in linear time, in any layout, and read string keys in place in every other sort; sorting the 1.3M-row laghos genesis counter frame by `metric` takes 39 ms instead of 481 ms ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `sort_by_multi` over one integer column that holds each row position once (such as a row index) place the rows directly instead of sorting, which takes the duql `window` stage from 382 to 324 ms on 1M events ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make the duql `group` fold look up a string key and a `count_distinct` string value without building a key string per row, which takes `group name { d = count_distinct(fname) }` from 93 to 56 ms on 1M events ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `View.from_file` and `View.from_files` use the default index beside the trace, as `View.from_directory` does, so they prune chunks, read the aggregation tier and cache lookups ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `filter`, `take`, `head`, `tail`, `slice`, `drop_nulls`, sort output and join payloads keep view, dictionary and selection columns instead of copying them flat, and make string predicates (`==`, `contains`, `starts_with`, `ends_with`, `matches`, `like`, `search`, `find`, `count`, `len_bytes`, `len_chars`), scalar compares and `in` lists run once per dictionary entry or in place on views ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make a View scan build each string column (`name`, `cat`, `fhash`, `hhash`, string and JSON args) as a dictionary over views when a batch has at most a quarter as many distinct values as rows and as a view column otherwise, pointing into the scan's string table instead of copying the bytes, and make `concat` of string columns in any layout give a view or dictionary column over the parts' buffers. Collecting the 1.3 M-row laghos genesis file peaks at 1.3 GB instead of 2.0 GB ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Change `to_arrow` of a View's string columns to give `string_view` arrays, or dictionary arrays of `string_view` values; `to_list`, `to_numpy`, `to_pandas` and `to_polars` give the same values as before ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `group_by` read a dictionary key once per distinct value per batch and a view key in place, without copying the key column ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Change the `dev` extra to require `pyarrow>=16`, the first release with view types ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Change the index to keep per-chunk evidence for every path whose evidence fits a byte cap, 5% of the file's compressed size and at least 8 MiB, instead of the 1,024 most frequent paths, and to drop evidence that cannot skip a chunk; `path_budget` is now an optional count ceiling, 0 by default.
  Existing indexes rebuild their zonemap, bloom and counts evidence once ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Size each chunk's bloom by its distinct values and record a path missing from a chunk without a bloom, so a test on the path's value skips that chunk ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Make `concat` keep dictionary columns as one dictionary column, and make the C++ plugin SDK and the JIT plugins read strings of dictionary and selection columns; a C plugin reads them with `dftu_series_string_at`, since `dftu_series_offsets` and `dftu_series_data` are null for such a column ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Change `dftracer_genesis_gen_dist` to write one record per run (`gtype` `run`, with the format `version`), per call path (`func`) and per call path and counter (`counter`), with no dftracer `ph`, `args` or envelope, and to store each distribution's DDSketch as base64 `sketch` beside its quantiles, so a file has a fixed set of JSON paths and distributions merge exactly.
  Old genesis files index as `dftracer` and must be written again ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change the built-in `genesis` record schema to read the new records by path, require `gtype` and `run` and store the `runs` row set, so `run -> runs.app` and `from runs` read run keys from the index ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Run a yes/no regex match on a value of 256 bytes or more on Vectorscan when its pattern has a `.*` or `.+` gap, such as `fname ~ "/scratch/.*\.h5$"`, which is 2 to 10 times faster on values of 1 to 4 KB ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Run the raw-line prefilter of a query on Vectorscan when a clause has 4 or more alternatives, such as `name in [...]`, which is 3 to 10 times faster on lines that do not match ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change a regex match that reaches the PCRE2 work limit to run again on Vectorscan, which gives the exact result where it gave unknown ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make a column expression over strings run as fast as the eager `Series.str` call: joining the parts of a chunked result copies each part's bytes whole instead of a row at a time (`concat_columns` of text, shared by every caller), so `with_columns(o=col("s").capitalize())` over 5 million short strings takes 0.0025 s, not 0.0875 s, and the peak memory of a string expression falls by 30 to 35 percent ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make the string predicates (`isalpha`, `isdigit`, `isalnum`, `isspace`, `islower`, `isupper`, `istitle`) stop at the first bad vector of each string when the mean length is 24 bytes or more, and keep the whole-buffer bit mask below that: over 100000 random strings of about 750 bytes they are 3 to 50 times faster than pandas (`isdigit` and `isspace` were level with it), and over strings that are all good they are no slower than before.
  `DFTRACER_UTILS_STRING_PREDICATE=bits` or `rows` forces one strategy ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `Series.where`, `Series.mask`, `DataFrame.where` and `DataFrame.mask` to take a null replacement by default, as pandas does.
  `DataFrame.where(cond)` and `mask(cond)` kept `0` where they changed a value; the series forms required the argument ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change a build over an index with a corrupt column family to fail with an error that names the index directory and says to delete it and build again.
  A corrupt aggregation tier is still cleared and built once more; when that does not cure the index, the error says so ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `group_by` over many groups to hold no array the size of the frame and to free each partition's state once its result is built ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make `group_by` give bit-identical results on every run and for every thread count: the chunked route folds into fixed lanes merged in lane order, the many-group route folds each group's rows in row order, and the packed route takes only aggregates whose bits do not depend on order.
  Before, a float sum, mean or std could differ in the last bits between two runs of the same call ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make the count, sum, min, max and mean cells of `group_by` hold only what the requested aggregates read, and reserve the per-group arrays from an estimate of the groups, so the many-group aggregation of 20 columns over 5 million rows and 200 thousand groups needs about 0.1 GB (sum, min, max) to 0.3 GB (with mean, std and count) above the frame, about what pandas needs ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make a variance or standard deviation in `group_by` keep its moments in three words of the packed group row, 32 bytes a column and group for a standard deviation alone and 56 with sum, min, max, mean and count, where a general state took 112, with the same bits, and stop a column batch after the first from joining key columns it drops ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make `group_by` over many groups write its aggregate and key columns straight into the output, at each group's final row, instead of building small columns for each of the 64 hash partitions and joining them: no per-partition columns are held at once and there is no concat or reorder copy, and the values, null masks, dtypes and group order are unchanged. Shapes it does not write (a temporal, decimal, binary or json key, a text, list or boolean aggregate column, order-dependent state) keep the old path. The column batches take 8 MB of cell state a pass instead of 32 MB, so at 200k groups a pass is one column; the environment variable `DFTRACER_UTILS_GROUPBY_STITCH` forces the old path for tests, and `agg_in_place_finalizes()` counts the group-bys that took the new one.
  Over 5M rows, 200k groups and 20 value columns the memory above the frame fell from 0.38 GB to 0.14 GB for sum, min and max and from 0.91 GB to 0.32 GB for sum, min, max, mean, std and count.
  Aggregates that keep moments over many columns run a few columns at a time.
  The sums of a many-group aggregation can differ in the last bits between runs, as they already could in the other many-group routes ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).

- Change `dftu_series_cut` to take a trailing `int32_t flags` (`DFTU_CUT_RIGHT`, `DFTU_CUT_INNER`; 0 keeps the old result), and the `dftu.series.cut` op to take it as a fourth operand.
  A caller of the function must pass the new argument; there is no old entry point ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `Series.rank` to leave a float `NaN` unranked, as it does a null: the row gets a null rank and is not counted by `rank(pct=True)`, where it took a rank like any value and moved the rank of every row after it ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `dftu_dataframe_join` and `dftu_lazyframe_join` to take a trailing `int32_t nulls_equal` (0 keeps the old rule), and the `dftu.frame.join` and `dftu.lazy.join` ops to take it as a seventh operand; `OwnedLazyFrame::join` takes `bool nulls_equal = false`.
  A caller of either function must pass the new argument; there is no old entry point ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `Series.replace` of a value its column type cannot hold (a number on a string column, a string on a numeric one) to leave the column unchanged, as pandas does, where it raised `TypeError`.
  A replacement the column cannot hold still raises ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `set_union` to give each value in full.
  A float prints as the shortest text that parses back to the same value, where it printed six decimals (`0.1234567891` became `0.123457`), and an empty string is kept, where it was dropped.
  A string value that contains the separator `\x1e` now fails with an error that names the separator and the group, where it read back as two values.
  A column of a type other than string, bool, integer, float or a list of those fails with an error that names the type.
  The trace View already printed floats this way; the dataframe engine now agrees ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `count` of a column inside an aggregation to count the group's non-null values, where it counted every row of the group.
  This covers `col("x").count()` in `group_by().agg` (default output name `count_x`) and the trace View's `F.x.count()`, which counts the rows where the field is present, as the View engine already ran a counted field.
  C++ and the C ABI gain `agg_count_valid` and `dftu_agg_count_valid` for the same aggregate.
  The group row count is unchanged: `count()` with no column, `F.any.count()` and the `"count"` spec.
  A mean rebuilt as `sum / count` of a column with nulls is now right; the workaround `col("x").is_not_null().sum()` is no longer needed.
  Two `F.x.count()` in one aggregation share the output name `count`, so give each a separate plan ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change a reduction the engine does not define to fail instead of returning 0: `sum` or `mean` of a string column, `sum` of a timestamp column, and a code other than sum, min or max passed to `dftu_series_reduce`.
  `dftu_series_reduce` returns a scalar tagged `DFTU_SCALAR_TAG_ERR` that carries a `dftu_error` naming the operation and the type, the C++ `Series` methods throw `DFTUtilsException`, and Python raises `TypeError`.
  Before, the call logged an error and returned 0 (the maximum, for a count or mean code).
  `dftu_scalar` gains the tag and its union a `err` member, so code that reads a scalar by tag handles the new tag ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change a duql string literal that holds a backslash before a quote or a doubled backslash to mean one character less.
  Any other backslash stays in the string, so regexes such as `"\d+\.h5"` read as before ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Remove the re-export of `F`, `Field`, `Expr` and `Value` from `dftracer.utils.duql`.
  Import them from `dftracer.utils` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change the engine `asof` tolerance (`DataFrame.asof`, `LazyFrame.asof`, `dftu_dataframe_asof` and the `dftu.frame.asof` op) to a double in the units of `on`.
  A fraction bounds a float time column, and integer times compare with its floor ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Remove the `DUQL_LOOKUP_MAX_ROWS` and `DUQL_LOOKUP_MAX_BYTES` caps from `in`, `not in` and sub-queries correlated by `==` keys.
  A top-level `k in (...)` in the leading `where` still filters the scan, and every other such term runs through the engine join, which spills to disk over the memory budget.
  Such a term runs after the scan, so the query cannot be exported with `sink_json`.
  `asof`, `overlap`, range-correlated sub-queries, arrows and uncorrelated scalar sub-queries keep the caps ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Remove the row and byte caps from the duql `lookup` stage, plain and `into`.
  A side that outgrows `DUQL_LOOKUP_MAX_ROWS` and `DUQL_LOOKUP_MAX_BYTES` is joined by the engine, spills to disk over the memory budget and is not cached ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Make a duql correlated sub-query that mixes `^.` with the sub-query's own fields on one side a compile error.
  It used to read both fields from the enclosing row ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Change `Series` and `DataFrame` pickles to the native frame format (`to_bytes`).
  Pickles of an earlier version do not load ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Change a list with mixed value types in `Series.from_list` to raise `ValueError` instead of a pyarrow error ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Change the duql lookup cache and the stored row sets of a source (the `from files` rows and the `file_path`, `host_name` and `rank` group keys) to a native frame format instead of Arrow IPC.
  Row sets stored by an earlier version read as stale and rebuild on the next index build.
  A cache entry in another format is recomputed ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Change a path-decoded schema to always index its duration, entity, lane and name fields, as it does for its time field ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change an index build to keep an array or object of more than 256 children as one JSON leaf.
  Records of huge arrays or maps add a bounded number of catalog paths and zone keys, and queries still read every element ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make duql and the DataFrame string kernels share one pattern engine.
  Literal patterns such as `like "abc%"`, `"%abc%"` and `"text" in x` run on a SIMD substring search.
  Other `like` patterns search their literal parts the same way.
  Regexes run on PCRE2, with its JIT where available, after a SIMD check for the literals every match needs.
  Case-sensitive `like` and regex filters also give the raw line pre-filter needles, so a View scan parses only the lines that hold them ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Limit the work of every regex match.
  A string that reaches the limit gives unknown in duql and null in a DataFrame kernel, instead of a hang or a crash ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make an `in` list over an args field prune chunks by their min and max, not only by bloom filters ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make the View schema come from the path catalog.
  The schema now lists a field that only some events of a name carry.
  Each column takes its type from every record, not from the first event of each name.
  A column can gain a wider type, such as `float64` where a later event holds a double ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make an `in` or `not in` list of 16 or more strings use a hash set.
  A filter with thousands of values now costs one lookup per event ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make views skip lines before they parse them when a line cannot hold the `==` or `in` values of the filter.
  On a 1M-event trace, a selective name filter runs in about half the time.
  `name` queries are 21 to 29 percent faster in `index_bench`.
  Filters without such values are unchanged.
  The reader uses the same test ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make the frame ops `window`, `gap_fill`, `asof` and `interval` native kernels that no longer convert to Arrow.
  They work in every build, with or without `DFTRACER_UTILS_ENABLE_ARROW`, and so do duql `session`, `window` and `lookup ... asof`.
  Results, C ABI entry points, op names and Python methods stay the same ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Move the C++ frame op types from `utilities::common::arrow` to `dataframe` in `dataframe/frame_ops.h`.
  The `WindowFunc` enumerators, such as `WindowFunc::RowNumber`, are now CamelCase ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Make the Python conversions work without pyarrow.
  This covers `Series.from_list`, `s[i]`, slices, `to_list`, `to_numpy`, `to_pandas()`, `to_polars()`, `DataFrame.to_dict`, `DataFrame.from_numpy` and `DataFrame.from_dict`.
  `from_arrow`, `to_arrow` and `to_pandas(arrow=True)` still use pyarrow ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Make `gap_fill` take an integer time column only.
  A floating time column was never reachable from `DataFrame`, the C ABI or Python ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Change the `sessionize` window function to measure the gap from the latest end of the session so far.
  A row with a null time now gets a null session.
  Before, it kept the current session number ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change the `dftu_window_spec` C struct and the C++ `WindowSpec` to keep the parameters that only some window functions read in one union.
  In C, the fields are `param.offset`, `param.frame`, `param.rate` and `param.session`.
  In C++, the type is `WindowParams`, with `window_spec`, `frame_spec`, `rate_spec` and `session_spec` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make times in duql and the View use the units of the record schema.
  This covers `time_range`, `bucket` widths and keys, `time_bucket_min`, `busy()` and `active()`.
  `call_tree` and `flamegraph` write `ts`, `dur`, `total` and `self` in the role units, as floats when a unit is not microseconds.
  `View::time_scale` on a path schema raises an error, so read the units from the schema fields ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a declared schema field convert values to its type.
  `"42"` and `42.0` become an `int`, number text becomes a `float` and any scalar becomes its JSON text for a `string`.
  A value that does not convert reads as null and is counted ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change a duql `let` without `from` to read `data`, as the main query does.
  Source row sets still read `all` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a column that a scan `select` dropped a compile error that names the columns left.
  Before, it was a column of nulls ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `phase` on a path schema raise an error ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change record schema detection to judge a schema on the records its `data` row set keeps.
  Metadata lines no longer match or miss a schema.
  Detection reads past leading metadata to 1000 records, up to 16 MiB of text ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make the dftracer schema require `ts` besides `ph` and `name`.
  Chrome trace events are dftracer, and a log with only `ph` and `name` is generic ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change schema choice when several schemas match.
  Equal required-field counts prefer a user schema over a built-in one.
  A schema whose fields are all optional is chosen when any declared path is present and no schema with required fields matches ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make schema paths resolve as queries read them, with flat dotted keys and array indexes ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `explain_file_schema` report `records`, the objects that no schema treats as metadata.
  It caches its result per file by path, size and modification time ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a View of many files take the schema of the files that have records.
  An empty file or a file with metadata only takes no vote.
  A View of such files only is generic, or dftracer when one file holds metadata ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `dftracer_server` pick its schema from every file instead of the first ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a View, `TraceViewer` and `dftracer_view` read the source `data` row set by default.
  dftracer metadata records (`ph` `M`) are no longer returned unless you ask for `all()` or `--all`.
  `phase("metadata")` still selects only metadata records ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Replace the recipe JSON key `include_metadata` with `all` (bool) ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change a bare field name to read `args.<name>` only when the source sets `def args_fallback = true`.
  The dftracer and genesis sources set it.
  For generic files, `x` no longer reads `args.x`, so write `args.x` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make the `rank`, `file_path`, `file_name` and `host_name` group keys read the `ranks`, `files` and `hosts` row sets ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `dftracer_view`, `dftracer_run`, `dftracer_index` and the plugin host write a gzip copy of a plain trace under `split/` and read it.
  Building an index for a plain file elsewhere now fails with an error that names the file.
  Before, it wrote an empty index ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make existing dftracer indexes rebuild on first use ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Rebuild indexes that were built before this release.
  The index catalog now lists empty arrays and objects, which `exists()` relies on ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change the stages after the scan to treat a missing field and a JSON `null` as the same null cell.
  `x is null` and `x is missing` are true for both, and `exists(x)` is true for neither.
  DataFrame masks follow the same rule.
  A leading `where` still tells them apart ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make an integer result of duql arithmetic outside int64 unknown, as in a column.
  `int(x)` gives an int64 ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `dftracer_view --save-recipe` refuse `--duql`.
  Write the filter into the recipe instead ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change regexes to use the duql dialect, the common subset of RE2, PCRE2 and Vectorscan.
  This applies in duql (`~`, `~*`, `!~`, `!~*`, `extract`) and in the DataFrame kernels `str_matches`, `str_search`, `str_extract` and `str_findall`.
  Backreferences, lookaround, atomic and possessive groups, recursion, callouts, `\C`, `\K` and `(*VERB)` are compile errors.
  A kernel returns NULL for them ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change `like` and `str_like` so that `%` matches newlines too and `_` matches one UTF-8 character instead of one byte.
  A `str_like` pattern with an inner `_` costs about 0.5 ns more per row ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change a duql `like` to have no escape character unless `escape` names one.
  Before, `\` was a literal character there ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change `str_like` so that a `\` before anything other than `%`, `_` or `\` makes the pattern invalid (NULL).
  Before, it was a literal.
  `str_like` keeps `\` as its escape character ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a string compared with an object or array unknown, so the filter drops the record.
  Write `json(x) == "..."` instead, which uses canonical JSON with no spaces and sorted keys.
  This includes fields that a record schema declares as `json` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change duql filters to use three-valued logic on every path.
  The paths are the JSON scan, the indexed reader, folds, the aggregation tier, plugins and DataFrame masks.
  A condition on a missing or null field, or across types, is unknown, and a filter keeps only true records.
  `x != 1`, `x not in [...]` and `not (x == 1)` no longer keep records without `x`.
  A field present as `null` no longer falls back to `args.x`.
  Integers and doubles compare exactly, without rounding to a double.
  A `DataFrame` mask on a missing column or a cell of another type gives null rows, which the mask drops.
  Before, a missing column threw an error.
  `Series` `&` and `|` use Kleene logic when either side has nulls, so `between(..., "neither")` is null at a null.
  See "Missing, null and unknown" in the duql reference ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Rename the filter language to duql on every surface.
  The old names are removed and have no aliases.
  In Python, rename the module `dftracer.utils.query` to `dftracer.utils.duql`.
  Rename `TraceViewer.query()` and the Dask `query()` to `duql()`.
  Rename `DFTUtilsQueryError` to `DFTUtilsDuqlError` and `Expr.to_query()` to `to_duql()`.
  Rename the `query=` argument of the dfanalyzer readers and `Indexer.explain(query)` to `duql`.
  In C++, rename the namespace `dftracer::utils::query` to `dftracer::utils::duql`.
  Rename the headers `dftracer/utils/query/` to `dftracer/utils/duql/` and the library `dftracer_utils_query` to `dftracer_utils_duql`.
  Rename `View::query()` to `View::duql()`.
  Rename `QueryError`, `QueryParseError` and `QueryErrc` to `DuqlError`, `DuqlParseError` and `DuqlErrc`, and the error code `QUERY` to `DUQL`.
  In the C ABI, rename `dftu_query` and `dftu_query_*` to `dftu_duql` and `dftu_duql_*`.
  Rename `DFTU_QCMP_*` to `DFTU_DUQL_CMP_*`, `DFTU_QMATCH_*` to `DFTU_DUQL_MATCH_*` and `DFTU_TOK_QUERY` to `DFTU_TOK_DUQL`.
  In the plugin ABI, rename `plugins/abi/query.h` to `plugins/abi/duql.h`.
  Rename `DFTU_SVC_QUERY` ("dftu.svc.query@1") to `DFTU_SVC_DUQL` ("dftu.svc.duql@1").
  Rename `query_compile` and `query_matches` to `duql_compile` and `duql_matches`, and the plugin field `plan_query` to `plan_duql`.
  Rename the plugin config key `"query"` (`--parg query=`) to `"duql"`.
  The plugin ABI fingerprint changes, so plugins built against the old headers fail to load and you must rebuild them.
  Rename the CLI option `--query` to `--duql` on `dftracer_view`, `dftracer_stats` and `dftracer_comparator`.
  `dftracer_info --query` keeps its name.
  Rename the server and web UI parameter `query` to `duql`.
  Rename the view recipe and comparator config key `"query"` to `"duql"`.
  `DataFrame.query`, `Indexer.query_file_*` and the query cache keep their names ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Reserve the words `null`, `is`, `between` and `escape` in queries.
  Write a field with such a name in backticks ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `-` always an operator, so `a-b` is a subtraction.
  A query that starts with a stage name, such as `sample == 1`, needs backticks around the field ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove the C++ functions `query::tokenize` and `query::parse_tokens` and the types `query::Token` and `query::TokenKind`.
  Use `duql::parse` instead ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Move rollups and materialized views to a query cache beside the index, in `.dftindex-cache/rollups` and `.dftindex-cache/views`.
  `DFTRACER_CACHE_MAX_BYTES` caps each store (default `2G`) and each store drops its least recently used entries first.
  A cached result also depends on the record schema, so a schema override does not reuse the result of another schema.
  Deleting `.dftindex-cache` changes no query result ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Stop reading rollups stored inside an index and old `.dftindex-views` folders.
  The next build clears the rollups inside an index.
  Delete `.dftindex-views` to free its space ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Store the aggregation tier as the `dftracer.agg` extension, with one manifest entry for each aggregated file.
  A changed `AggregationConfig` or a changed aggregated trace clears only the tier and aggregates every file again, instead of deleting the whole index.
  Members, the bloom tier, plugin extensions and rollups stay.
  Only an index in another format is rebuilt whole.
  Queries use the tier only when every file of the view has a current entry and read the traces otherwise.
  `IndexStatus::aggregation_needs_rebuild` now means the next build rebuilds the tier ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Rebuild aggregation tiers built before this release on the next aggregation build ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Replace `AggregationConfig::compute_hash` with `params_hash` (64 bits) ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Replace `IndexDatabase.write_agg_global_config` and `IndexDatabase.write_agg_file_markers` in Python with `write_agg_config(aggregation_config)`.
  Each file's entry comes with its SSTs ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `EventAggregator` take no config hash ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Rebuild plugins built before this release.
  They fail to load with an ABI version error because the plugin ABI fingerprint changed ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Change `dftracer_genesis_gen_dist` records to carry only the `run` id.
  `app`, `system`, `unique_input`, `nodes`, `ppn` and `papi_set` are on the run's `RUN` line alone.
  Read them as `run -> runs.<key>` instead of `args.<key>` and regenerate existing output to use the new layout ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `GET /api/viz/events` stop returning metadata records, which the viewer never used.
  A request whose filter rules out every chunk now answers in about 10 ms instead of 110 ms on a 1.4M-event trace ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make views and the reader drop lines before parsing them when a `==`, `<`, `<=`, `>` or `>=` filter on a number cannot hold for any value written after the field's key.
  On a 1.4M-event trace a viewer time window request goes from 115 ms to 98 ms ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `GET /api/viz/untimed` sort the records of a query once and serve every later page from the server's result cache.
  A later page takes about 1 ms instead of 100 ms ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make the viewer's density requests read the time before a zoomed-in window in one scan instead of two.
  Cold zoom-ins of a 1.4M-event trace are about 30 percent faster with the same result ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make an arrow filter that matches more than 4096 keys skip pruning.
  It still returns the same rows ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make an index build write the evidence of a file one chunk at a time.
  On a trace with 4000 args paths the peak memory of an unbounded build drops from about 1.5 GB to 885 MB ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Bound automatic evidence with a path budget.
  Besides the fixed fields and the named ones, the `path_budget` (default 1024) most frequent args paths of each file get a zone map and a bloom.
  The other paths can still be filtered on but never skip a chunk.
  `BloomOptions::auto_fields` and `ChunkIndexerConfig::auto_fields` become `path_budget`.
  Python `BloomConfig(auto=...)` becomes `BloomConfig(path_budget=...)`.
  `dftracer_index --no-auto-dimensions` becomes `--path-budget 0` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make each pruning extension record its own configuration hash.
  A changed bloom setting rebuilds only `bloom` and a changed value-count cap rebuilds only `counts` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make a View filter that uses only `ts` prune chunks by their time range instead of reading every chunk ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make a View filter on a file path or host name, such as `fhash -> files.path == "/data/a"`, skip the chunks the index rules out.
  Before, it read every chunk ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Change the index on-disk layout to a new format version (1).
  Existing indexes are rebuilt once on the next build.
  The aggregation, system-metrics and rollup data are unchanged ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `build` rebuild the bloom tier when its false positive rate, expected entries, auto-field settings or value-count cap change.
  Before, it compared only the indexed fields.
  Asking for fewer fields still does not rebuild.
  `Indexer::build` and the Python `Indexer` now rebuild an index from an older schema ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
  Before, only the reader and the server checked the version.
- Add a trailing `truncated` field to `dftu_indexer_report` and `dftu_indexer_file` in the C ABI ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Rename C++ `index::gzip::Indexer` to `index::gzip::CheckpointIndexer` and `IndexerFactory` to `CheckpointIndexerFactory`.
  They live in `index/gzip/checkpoint_indexer.h` and `checkpoint_indexer_factory.h` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make Python `Indexer.build()` and `ensure_indexed()` run on `Indexer` and raise when a trace cannot be read.
  Before, they left the trace in `needs_work`.
  `dftracer_event_count` fails the same way.
  Python `CheckpointIndexer.need_rebuild()` and `exists()` raise on an error instead of returning `True` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Change the index module to use `ankerl::unordered_dense` maps and sets instead of `std::unordered_map` and `std::unordered_set`.
  This includes the results of the `IndexDatabase` query calls, where string keys use `StringViewMap` and `StringViewSet`.
  Iteration order is now insertion order.
  Any later insert or erase invalidates a reference into one of them ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Move the indexing code into one module, `dftracer/utils/index/`, with a namespace for each layer.
  The layers are `index/store`, `index/gzip`, `index/extensions`, `index/schemas/dft`, `index/build`, `index/plan` and `index/cache`.
  `trace/indexing/indexing.h` is now `index/index.h`.
  Only headers that another public header or the documented build workflow needs stay installed under `include/dftracer/utils/index/`.
  The index classes drop the `Utility` suffix, so `ChunkIndexerUtility` is `build::ChunkIndexer`, `IndexBatchBuilderUtility` is `build::BatchBuilder`, `IndexResolverUtility` is `build::Resolver`, `ChunkPrunerUtility` is `plan::ChunkPruner`, `AggregatorUtility` is `schemas::dft::agg::Aggregator` and `DftracerTraceWriterUtility` is `schemas::dft::agg::DftTraceWriter`.
  No forwarding headers or namespace aliases remain ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make a duql duration beside a product or quotient of a timed field an error.
  Before, `where dur / 1000 > 0.5ms` compared `dur / 1000` with 500 and matched nothing ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make a missing or JSON `null` group key in View trace `group_by` its own group, shown as null.
  Before, it merged with `""` and showed as `""`.
  The same holds in counter export, the `ViewSession` join and rollups.
  Old rollups are recomputed because the rollup format changed ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a line longer than the read buffer an error instead of the silent end of the stream ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a View scan that cannot read a file's metadata, such as a plain file or a missing trace, raise an error with the cause.
  Before, it returned no rows ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Prune path-decoded files by zone map for equality and `in` filters on fields named `name`, `pid`, `ts` or another dftracer field.
  `exists()` on these fields now prunes by the catalog ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make plugins read path-decoded files with the file's own column names in `on_batch`, `transform`, `trace_read` and duql matches.
  Before, they got dftracer columns such as `name` and `args.x` that the records do not have ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make the window functions `running_sum`, `frame_sum` and `delta` exact for integer columns.
  Over an unsigned column, `running_sum` and `frame_sum` give `uint64`.
  A result outside its type is an error that names the function.
  Before, a `uint64` value at or above 2^63 or a sum outside `int64` wrapped ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Make integer `sum` in a group aggregation stay an integer and give null when the sum is outside the type.
  This holds for Int64 and Uint64, spilled partials included.
  Before, the sum wrapped ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `merge_shard_set` refuse shards that were aggregated with different configs.
  Before, it merged their rows ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `dftracer_view --time-range min,max` exclude `max` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Replace `IndexDatabase::resolve_name_to_hash` with `resolve_name_to_hashes`, which returns every hash for a name ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Change the C++ `WindowColumn` to read its column names through `value()`, `time()` and `end()`.
  Set them through `set_value`, `set_time` and `set_end` after `func`, and an empty name means no column ([cd9b4e3](https://github.com/llnl-asr/dftracer-utils/commit/cd9b4e3d6d2d)).
- Move the per-morsel name state of a C++ `Morsel` behind `dyn_state()` ([cd9b4e3](https://github.com/llnl-asr/dftracer-utils/commit/cd9b4e3d6d2d)).
- Store the index pruning evidence as the extension kinds `zonemap`, `bloom`, `counts` and `postings`, and move the other dftracer statistics to `dft.stats`.
  Chunk decisions do not change ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Change a group-by over many groups to aggregate each hash partition of the rows alone, so the state is one copy of the groups instead of one copy per thread.
  The partitioned driver now runs for any integer key and for a string key of any length, above 262,144 rows with at least 2,048 groups in a probe, where only a string key of at most 16 bytes took it.
  Measured on 20 nullable value columns with a string and a small integer key: 5,000,000 rows and 200,000 groups took 1.36 s and 7.10 GB above the process level, now 0.22 s and 0.38 GB; 1,000,000 rows and 200,000 groups took 0.61 s and 4.13 GB, now 0.08 s and 0.22 GB.
  The result, its values and its group order are unchanged.
  `benchmarks/groupby_analyzer_bench.py` measures time and peak memory in a fresh process per case and has a `--gate` mode ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `to_pandas`, `to_numpy` and `to_list` of integer, float, bool and string columns to convert in one native pass.
  `to_pandas(nullable=True)` of 20 numeric columns with nulls at 5,000,000 rows takes about 55 ms where it took 129 ms to 1.8 s, a bool column with nulls 2 ms, and the benchmark's 22-column frame 50 ns per row (0.25 s at 5,000,000 rows, 1.94 s before).
  `to_list` of 5,000,000 integers with nulls takes about 56 ms, near the 50 ms Python needs to build such a list.
  Rows with the same text share one `str` object, so a column of repeated names allocates once per distinct name.
  `to_pandas()` of a float column with nulls costs about 0.7 ms more per 5,000,000-row column than before; the nullable form is faster ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `materialize` of a dictionary or selection column to keep the nulls the view itself holds.
  A dictionary-encoded string column with a null read back as an empty string through every conversion, where the null stayed null before the dictionary was built ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Change `partial_arrow_view_groupby` in `dftracer.utils.dfanalyzer` to emit `{c}_m2`, `{c}_mean_hi` and `{c}_mean_lo` in place of `{c}_sumsq`, and `finalize_view_partials` to derive `std` from `m2`.
  A variance rebuilt from a sum of squares lost every digit when the mean was large next to the spread (durations near 1e9 with a spread of 3 gave a relative error of 1.0).
  Merge the partials of a view row with the new `merge_view_partials(df, full_cols, sum_cols, min_cols, max_cols, set_cols, flatten_fn)` in place of a `groupby().agg` dict; it merges the moments with the pairwise formula and the sum, min, max and set columns by their own rule, in the partials' column order. A caller that merged `{c}_sumsq` with a plain sum now fails with a `KeyError` naming the column ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Group the guides by task into seven sections that open from the guides page and collapse in the sidebar.
  Update the guides, tutorials and API reference to the API of this release ([98bba74](https://github.com/llnl-asr/dftracer-utils/commit/98bba745c6d5)).
- Version the plugin ABI, the index format and duql by hand in the new `VERSION` file (`abi: 0.0.1`, `index: 1`, `duql: 0.1`), in place of a hash of the ABI headers.
  A comment edit in an ABI header no longer rejects every plugin.
  Rebuild your plugins against this release, because the host loads only a plugin with the same ABI version ([c329c07](https://github.com/llnl-asr/dftracer-utils/commit/c329c07237e7)).
- Make a duql query declare its version as `duql 0.1`, or `duql 0` for the major version alone.
  A query runs when its major version equals the engine's and its minor version is not greater.
  A query that declares `duql 1` now fails, so write `duql 0.1` or drop the line ([c329c07](https://github.com/llnl-asr/dftracer-utils/commit/c329c07237e7)).
- Take the release version of a CMake build from the git tag, or from scikit-build when it builds the wheel, in place of a version written in `CMakeLists.txt` ([c329c07](https://github.com/llnl-asr/dftracer-utils/commit/c329c07237e7)).

### Added

- A provenance graph extractor and a `/api/prov/graph` endpoint in the viz
  server. It assembles entities and activities from dftracer provenance-mode
  records and attributes POSIX/STDIO I/O to activities, giving the files each
  one touched and the mount points holding them (`io`, `all_files` and
  `mounts` query parameters).
- A Provenance tab in the web viewer that draws the lineage graph with
  Cytoscape and filters by entity, file and storage.
- Add `append_fmt` and `append_json` to `GzipLineWriter` and its producers: a line built from a format with `{}` placeholders straight into the member buffer, compile-time checked (`append_json<"...">(args...)`, format and argument count checked when compiled) or parsed once at run time (`LineFormat::parse`, `JsonLineFormat::parse`). Arguments are type-checked at compile time in both forms; `append_json` JSON-escapes strings. Also `append_with` for producers that format into the buffer themselves, an unordered mode with per-worker `Producer`s, and a `memory_budget` option ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Add `GzipLineWriter` (`utilities/fileio/gzip_line_writer.h`) and its blocking adapter: appended whole lines become a multi-member gzip file, cut at line ends at or after the member size, compressed in parallel with bounded memory and written in line order, with each member's offsets and line numbers reported in order, optional part files and a per-member hook. A failed or abandoned writer removes its output. The writers of this library move to it in later changes ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Write a trace's path catalog during its first full scan: a collect or streamed query with no filter, time window or row limit on a trace indexed only by a query records every path it decodes and writes the catalog at the end, so `View.columns`, `schema_tree` and duql wildcard paths see every field without a `dftracer_index` build. It costs nothing measurable on the laghos trace and about 200 ms once on a trace with 200 counter paths ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Add wildcard path segments to duql: `*` in place of a key matches one segment, in `select`, `drop`, `unpivot` and as the argument of `any(...)`/`all(...)` in a comparison (`where any(args.counters.*.p50) > 92`). A pattern expands when the query is planned to every matching path of the index catalog, so pruning sees each one; a pattern with no match, or a trace whose index holds no catalog (build it with `dftracer_index`), is an error ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add `DFTRACER_UTILS_SPILL_DIR`, the directory of every spill file (collect, sort, group-by, unique and join); unset, the writable node-local disk mount with the most free space is used, else the system temp directory. Point it at local disk on nodes where `/tmp` is held in memory ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add `<`, `<=`, `>` and `>=` against a string scalar, by byte order, the order sort and column-to-column compares use; before, only `==` and `!=` worked ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add the view encoding for string and binary columns (`Series.encoding` 4): 16 bytes per row, values of up to 12 bytes inline and longer ones in buffers the column keeps alive, so filter and take copy no string bytes. `dftu_series_new_string_view` builds one over caller buffers without a copy ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Read and write Arrow `string_view` and `binary_view` arrays with zero copy, in `Series.from_arrow`, `DataFrame.from_arrow` and `to_arrow`, including dictionaries with view values ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add `dftu_series_string_at`, which reads one string of a flat, dictionary or selection column, and `dftu_series_buffer_bytes`, the bytes a column holds with its children ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add `stats_share` to set the evidence cap, as `index.stats_share` in a record schema, `dftracer_index --stats-share`, `BloomConfig` and `BatchIndexer` in Python, `BloomOptions` in C++ and `bloom_stats_share` and `bloom_path_budget` in `dftu_indexer_options` in the C ABI ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Add `duql::Source` to the C++ builder and `Source` to the Python one, which build the row sets, macros and flags of a record schema's duql source, such as `Source().rowset("data", Pipe().where(c("ph") != "M")).flag("args_fallback", true)`; the built-in `dftracer` and `genesis` sources use it ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add hopping buckets to the duql `bucket` stage, such as `bucket 5s every 1s | agg { b = sum(size) }`, which put each record in every window that holds it, and `at` to align buckets to an origin, such as `bucket 1h at 30m`, with `every` and `at` in the Python and C++ builders ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql array index be a parameter or any expression, such as `xs[$n]` and `xs[i - 1]`, and a list literal stand anywhere an expression goes, such as `derive t = [cat, name]` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql array functions `index_of`, `sort`, `unique` and `join`, and the index form `c("xs")[c("i")]` to the Python and C++ builders ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql calendar functions `date_part(t, part)` and `format_time(t, fmt)`, in UTC, such as `group h = date_part(ts, "hour") { n = count() }`, and `now()`, the time the query compiles in the record's time unit, such as `where ts > now() - 1h` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `Series.dt_format` and the pandas `Series.dt.strftime` in Python, `Series::dt_format` in C++, `dftu_series_dt_format` and the expression builders `dftu_expr_date_part` and `dftu_expr_format_time` in the C ABI ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql expression after the scan take a column where it took only a literal: the needle of `starts_with`, `ends_with` and `contains`, `from` and `to` of `replace`, `start` and `length` of `substr`, the digits of `round` and the group of `extract`, such as `lookup runs on run | derive p = starts_with(fname, prefix)` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add column forms of these expressions to the dataframe expression layer in C++ and the C ABI, and let the Python column expressions `starts_with`, `ends_with`, `contains` and `replace_all` take an expression argument ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql `window` block run `count_if`, `count_distinct`, `collect`, `arg_max` and `arg_min`, over the whole partition or a frame, such as `window rank sort ts { n = count_distinct(fname) over 3 rows }` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `var`, `std` and `quantile` in a duql `window` block take an `over N rows` or `over d` frame; a framed `quantile` is exact ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the window frame functions `FRAME_VAR`, `FRAME_STD`, `FRAME_QUANTILE`, `FRAME_COUNT_DISTINCT`, `FRAME_ARG_MAX`, `FRAME_ARG_MIN` and `FRAME_COLLECT`, with a quantile level and an ordering column in the frame spec, in C++, the C ABI and the Python `frame_*` window specs ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add duql pipeline macros: a `def` whose body is a run of stages, such as `def io_rate(d) = where cat == "POSIX" | bucket d | agg { b = sum(size) };`, runs where a query names it as a stage, such as `from data | io_rate(1s) | sort b`, and `Pipe::define` and `Pipe::use` and their Python forms build them ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql `parse` stage, which adds a column for each named group of a regex, such as `parse name ~ "(?<op>[a-z]+)\d*_(?<fd>\d+)"`, and `Pipe::parse` and `Pipe.parse` to the builders ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql function `regex_replace(s, re, to)`, which replaces every match of a regex and takes `$1`, `${name}` and `$$` in the replacement, such as `regex_replace(path, "/+", "/")` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `Series.str_regex_replace`, `Series.str.regex_replace` and the expression `regex_replace` in Python, `Series::str_regex_replace` in C++ and `dftu_series_str_regex_replace` and `DFTU_STR_FN_REGEX_REPLACE` in the C ABI ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `fill forward` and `fill linear` to the duql `bucket` stage, such as `bucket 1s fill forward | group name { bw = mean(size) }`, which carry or interpolate each group's values into the empty buckets ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `bucket d fill from lo to hi` fill a fixed range, such as `bucket 1ms fill from 0 to 6ms` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `over N rows` and `over d` frames to `sum`, `mean`, `min`, `max` and `count` in a duql `window` block, such as `window pid sort ts { bw = mean(size) over 1s }`, and `Col.over_rows(n)` and `Col.over(width)` in the C++ builder and `.over(rows=n)` and `.over(width)` in the Python one ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql window functions `running_min`, `running_max`, `running_mean`, `ntile`, `nth`, `percent_rank`, `cume_dist` and `fill_forward`, and a default for `lag` and `lead` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add a frame mode to `dftu_window_spec` so a C ABI window can ask for a range frame, and a trailing `"rows"` or `"range"` element on the Python `frame_*` window specs ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `inner` and `anti` to the duql `lookup` stage, such as `lookup runs on run inner`, which keeps only the rows with a match ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql `lookup` read an inline side, such as `lookup (from data | where type == "run" | select run, app) on run` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add Vectorscan to the build, which CMake downloads with Ragel and Boost headers when the system lacks them ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `DFTRACER_UTILS_ENABLE_VECTORSCAN` to build without it ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql `group` or `agg` entry call an aggregate a loaded plugin registers, such as `group name { e = myplug.entropy(dur) }` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql call a function a loaded plugin registers, in a `derive` or `select` entry, such as `derive e = myplug.entropy(x, 8)` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql `call` stage, which runs a table function a loaded plugin registers, such as `| call myplug.head(3)` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql `group` or `agg` entry be an expression over aggregates, such as `group name { ms = sum(dur) / 1000, r = sum(size) / count() }` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the expression forms of the string methods `Series.str` already had: `capitalize`, `title`, `swapcase`, `casefold`, the predicates `isalnum`, `isalpha`, `isdecimal`, `isdigit`, `islower`, `isnumeric`, `isspace`, `istitle`, `isupper`, the maps `zfill`, `pad`, `pad_start`, `pad_end`, `ljust`, `rjust`, `center`, `removeprefix`, `removesuffix`, `repeat`, `slice_replace`, and `split`, `rsplit`, `partition`, `rpartition`, `extract`, `findall`, `match`, `join`, `cat`, `get`, `index`, `rfind`, `rindex`, with the same values, nulls and types as the eager call.
  `rsplit` scans from the left, as the eager `Series.str.rsplit` does, so a separator that overlaps itself splits as `split` does.
  `get_dummies` (a column per token) and `cat()` with no other column (an aggregate) raise `NotImplementedError` and name the eager form; `count(pat)` keeps the aggregate `count()`.
  `col("b").cast("string").capitalize()` gives `True` and `False` for a bool.
  New C++ `expr_str_fn`, C `dftu_expr_str_fn` and `StrMapOp::Capitalize`, `Title`, `Swapcase` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Make the string kernels scan the whole data buffer with SIMD (Highway, chosen at run time) and build their output without a string per row: the case maps, the character classes (`isalpha` ..), `split`, `partition`, `rfind`, `pad`, `zfill`, `center`, `repeat`, `removeprefix`, `removesuffix`, `slice`, `cat` and `join`.
  A case map of a LargeString column now gives a LargeString (as `lower` and `upper` did); a result past what int32 offsets reach returns an error where the old kernels wrapped.
  The environment variable `DFTRACER_UTILS_STRING_SCALAR` runs the byte-at-a-time reference for tests and benchmarks ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `DataFrame.eval` forms for `.fillna(x)`, `.where(cond, other)`, `.mask(cond, other)`, `.abs()`, `.clip(lower, upper)`, `.round(n)` and the `na=`, `case=` and `regex=` keywords of `.str.contains`.
  `where` and `mask` without `other` give null.
  A method with no engine form is named in the error ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `with_columns(z=None)`, which adds an all-null String column ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).

- Add `Series.cut(breaks, right=, outer=)`: `right=True` closes each interval on the right and `outer=False` gives null outside the interior intervals and numbers them from 0, so `cut(breaks, right=True, outer=False)` equals `pandas.cut(labels=False)`.
  The default is unchanged ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `fillna` for bool columns (a boolean or a number, true unless zero) and string columns (a string).
  `with_columns(flag=True)` and `with_columns(name="k")` now build their column natively, with no Python list ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add a SIMD path for the number to bool cast of int32, int64, float32 and float64 columns without nulls.
  Over 20 million values `astype("bool")` of an int64 column went from 45 ms to 2.5 ms, and of a float64 column from 57 ms to 6 ms; a column with nulls still takes the scalar loop ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the typed result of `set_union`: `col("x").set_union(typed=True)` returns a list column of the group's distinct non-null values in their own type (`string`, `bool`, `int64`, `uint64` or `float64`), ascending, with an empty list for a group of nulls and no limit on the bytes of a string.
  C++ `agg_set_union(value, out, typed)`, the plugin `agg::set_union(value, out, typed)` and the C ABI `dftu_agg_set_union(value, out, typed)` carry the same flag, and the lazy group-by schema reports the list type.
  The default text form, its order and its separator limit are unchanged; the trace View keeps only the text form.
  A caller of `dftu_agg_set_union` must pass the new `int32_t typed` argument ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `nulls_equal` to `merge` and `join` (`DataFrame`, `LazyFrame`, `pandas.merge`; C++ `DataFrame::join` and `LazyFrame::join`; the C ABI).
  A join does not match a null key with a null key (the SQL rule), and that stays the default; pandas does match them, so a pandas `merge` over keys with nulls gives more rows.
  With `nulls_equal` a null key cell matches another null key cell in the same key column and never a value, for the inner, left, right, outer, semi and anti kinds, on eager and lazy frames and on a join that spills.
  It is refused for cross, lookup and nest.
  A join with the flag is run by the host: it is not offered to a source's join pushdown, and the build keys do not narrow the left scan.
  Flag off, the join is as fast as before; flag on, a one-million-row left join takes about 3.5 ms where it took about 2 ms, since it uses the generic path ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `df[["a", "b"]] = other[["x", "y"]]` assign several columns at once from a frame, and `df[["p", "q"]] = [s1, s2]` from a list of Series.
  The value's columns pair with the targets by position (its own names are ignored, as in pandas), a target that does not exist is added, and a count or row mismatch raises `ValueError` before any column changes.
  `df.loc[mask, ["a", "b"]] = frame` writes the selected rows.
  Before, a frame raised `TypeError: cannot assign <DataFrame> to column 'a'` and a list of Series read the list as one column of values ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `Series.replace` and `DataFrame.replace` take lists, dicts and null: `replace([inf, -inf], pd.NA)`, `replace({1: 2, 2: 1})` (a swap), `replace([1, 4], [10, 40])`.
  A new value of `None` or `pd.NA` makes the value null and keeps the column type; a NaN makes it NaN; an old `None` or `pd.NA` matches the nulls and an old NaN the NaN values.
  A float into an integer column widens it, and a replacement the column cannot hold (a string into a numeric column) raises `TypeError` that names the type, and the column in the frame form.
  Before, `Series.replace` took two scalars and nothing else ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `DataFrame.eval(expr)`: formulas over the columns as text, such as `df.eval("m = a / b")` for a new frame or `df.eval("a / b")` for a Series.
  It lowers the text with the same code as the source tier of `apply` and never runs it as Python.
  Several lines run in order, each seeing the columns the lines before it made, and a last expression line returns its value.
  `&` and `|` mean `and` and `or` as in pandas eval, so `a > 1 & b < 2` is `(a > 1) and (b < 2)`; `~` negates a mask; `x.str.contains("a|b")` is a regex search and `x.isna()` is the null mask.
  It supports column names, number literals, `+ - * /` with a number on either side, `//` and `%` (integers stay integers), `**` with a constant exponent, unary `- +`, comparisons and chains, `and`, `or`, `not`, `x if c else y`, `in` and `not in` a list of constants, `is None`, `abs`, `min`, `max`, `int`, `float`, `round`, `len`, `math.sqrt`, `log`, `exp`, `floor`, `ceil`, and the string methods `lower`, `upper`, `strip`, `lstrip`, `rstrip`, `startswith`, `endswith`, `replace` and `contains`.
  An unknown column raises `KeyError`; anything else with no engine form, such as `@name` or attribute access, raises `TranspileError` that names it ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Import an empty list or a list of only `None`, with no type, as a `string` column of nulls in `DataFrame.from_dict`, the `DataFrame` constructor and `Series.from_list`.
  This is the type `DataFrame.from_pandas` already gives a column of `None`; cast it for another type.
  Before, they raised `ValueError: unsupported Arrow column` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `Series.where` and `Series.mask` take a null replacement (`None` or `pd.NA`): the rows it selects become null and the column keeps its type.
  Before, `where(cond, None)` raised `TypeError: other must be a Series or a number` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `Series.clip` take one bound, `clip(lower=0)` or `clip(upper=b)`; a null stays null.
  `clip()` with no bound raises `TypeError` that says to pass a bound, where a one-sided call raised `clip() needs both a lower and an upper bound` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `DataFrame.astype` and `DataFrame.cast` take one dtype for every column, `df.astype("float64")`.
  A column that cannot be cast raises `TypeError` that names the column and both types and returns no frame; a mapping still casts only the named columns ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let `with_columns` and `assign` take a Python scalar or `lit(value)` as a column: `df.with_columns(z=1)` adds a column of the frame's row count holding the value, and an empty column for a frame with no rows.
  Before, `z=1` raised `AttributeError` and `z=lit(1)` raised `ValueError: a constant columnar expression needs at least one column`.
  The value is the same in every row whatever the other columns hold: a null, a NaN or an infinity in another column does not change it, and the column has no nulls ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a `filter` expression compare a column with another column: `df.filter(col("a") >= col("b"))`, eager or lazy, with `==`, `!=`, `<`, `<=`, `>` and `>=`.
  A row where either side is null is left out, numeric columns of different types compare after promotion, and two columns that cannot compare fail with an error that names both types.
  Before, the expression raised `TypeError: comparison right side must be a scalar value`; the Series mask `df[df["a"] >= df["b"]]` was the only form.
  Such a comparison is not pushable to a trace scan, so `to_duql()` raises `TypeError` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `pct=True` to `Series.rank`, which divides each rank by the count of non-null values (a percentile rank in (0, 1]); `method="dense"` divides by the number of distinct values, as pandas does.
  `dftu_series_rank` and `Series::rank` gain the option as a `flags` argument (`DFTU_RANK_FLAG_DESCENDING` = 1, `DFTU_RANK_FLAG_PCT` = 2): a C caller passing 0 or 1 for `descending` is unchanged, and any other nonzero value now means something else ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `DataFrame.sort_index(axis=1)`, which returns the frame with its columns in ascending name order (descending with `ascending=False`); `axis` is keyword-only and `axis=0` is unchanged ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `DaskFrame` in `dftracer.utils.dask`, a partitioned `DataFrame` over Dask futures, or in memory when no client is given ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- `DaskFrame` gains `agg(exact=True)` (median, quantile, distinct count and set union in one call), `map_partitions(overlap_next=)` for forward-looking windows, a two-stage shuffle for large partition counts (`shuffle(stages=)`, chosen above 1,024 splits), a `join` that picks the shuffle for `right` and `outer`, and a `nulls_equal` pass-through that raises until the engine's `join` has the option.
  It runs `map_partitions` (with an `overlap` of rows from the previous partition so `rolling`, `diff` and `shift` are exact across seams), a tree `reduce`, `group_by().agg()` for `count`, `sum`, `min`, `max`, `sumsq`, `mean`, `var` and `std` from per-partition partials, `shuffle` by key, and `join` with a broadcast or a shuffle strategy.
  After a shuffle every key is in one partition, so an exact median, quantile, distinct count or set union per group needs no merge of partial states; an aggregate that does not combine is refused with an error that says to shuffle first.
  The in-memory mode gives results equal to the cluster mode.
  It is Python only: no C++, C ABI or operation matrix change ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `nullable=True` to `DataFrame.to_pandas` and `Series.to_pandas`, which gives pandas' own nullable dtypes for integer, float, bool and string columns (`Int8` to `Int64`, `UInt8` to `UInt64`, `Float32`, `Float64`, `boolean`, `string`).
  A null is `pd.NA` and a float NaN stays NaN, where the default turned an integer column with nulls into `float64` with NaN.
  It is built without pyarrow, the default is unchanged, and it is an error together with `arrow=True` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `index=[...]` to `DataFrame.to_pandas`, which moves the named columns into the pandas index in the order given (a MultiIndex for several names).
  An unknown name raises `KeyError` and a repeated one `ValueError`.
  The native frame still has no row index ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add casts from integer, float and bool columns to string and from integer and float columns to bool.
  A column expression can cast to text: `col(x).cast("string")` (it raised `KeyError: 'string'` before) gives the same digits and floats as `astype`, with a bool as `true` or `false`, the engine's spelling.
  `astype("string")` gives decimal digits for an integer, the shortest text that parses back for a float with a point or exponent added (`2.0`, `1e+21`), and `True` or `False` for a bool (`true` or `false` from the engine and the C++ and C APIs); a null stays null.
  `astype("bool")` gives false for zero and true for any other value; a float NaN and a null stay null.
  A column cast to its own type is returned as it is, and any other pair, such as string to bool, raises `TypeError` that names both types, where it raised `RuntimeError: dataframe kernel produced a null column`.
  `Series.astype` also accepts the Python types `str`, `int`, `float` and `bool` and NumPy scalar types and dtypes that name a supported type; any other argument raises `TypeError` that lists the accepted forms.
  `dftu_series_cast` still returns NULL for a refused pair ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql `group`, `agg`, `window` or `pivot` block entry leave out its name, such as `group name { count(), sum(dur) }`, which gives the columns `count` and `sum_dur`.
  Python `Pipe.agg` takes such aggregates as positional arguments ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add the duql `case { c => a, else => d }` block, the same as `case(c, a, d)`, and `case_` to the Python and C++ builders ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `distinct` name its keys as `group` does, such as `distinct c = cat, dur // 10 as d` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `pivot k in ["a" as x]` name a value's column, so later stages need no backticks.
  With several aggregates the column is `agg_x` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add duql `take a..b`, which keeps rows `a` to `b`, counted from 1 ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let a duql parameter hold a list that `x in $p` and `x not in $p` read, and let `x like $p` read a string parameter.
  `TraceViewer.duql` binds a Python list or tuple, and the C ABI and `dftracer_view --param` read a list such as `["read", "write"]` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add arithmetic with `+`, `-`, `*` and `/` over numbers, parameters and durations to the `time_range`, `bucket` and `session` bounds of duql, such as `time_range $t0 .. $t0 + 10s` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `time_range lo ..` and `time_range .. hi` leave one side open ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `bucket d as name` name the key ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `sample $n seed $s` take parameters ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add duql `union name` and `union "file"` to read a row set, a `let` binding or a file ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Improve duql messages to print a parameter with its `$`, to name the argument count a function takes and to list every `as_time` unit ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add escapes `\\`, `\"`, `\'`, `\n`, `\t`, `\r` and `\uXXXX` to duql string literals, so any string can be written.
  The printers write double quotes with escapes ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let the web viewer filter on a name that holds a quote or a backslash ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add a duql builder in Python and C++.
  `dftracer.utils.duql` holds `source`, `rowset`, `Pipe`, `c`, `lit`, `param`, `duration`, `tup`, `sub` and `fn`, with one `Pipe` method per stage, `let`, `define`, `bind`, `text()` and the terminals `collect`, `count`, `first`, `stream`, `explain` and `on(viewer)`.
  C++ has `duql::Pipe`, `duql::Col` and the same factories in `duql/builder.h`, and `View::duql(const duql::Pipe&)`.
  A builder query parses to the same tree as the equal text ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql `lookup ... asof t within d` take a duration such as `5ms`, a fractional number or a parameter.
  A duration converts to the unit of the time or duration role of `t`, as in a comparison.
  Before, only a whole number in raw time units or a parameter parsed ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Let duql correlated sub-queries use range bounds.
  Besides `==` keys, a correlated `where` may compare one expression of the sub-query's row with the enclosing row by `<`, `<=`, `>`, `>=` or `between`, such as `derive m = (from data | where ts < ^.ts | agg { m = max(dur) })`.
  The sub-query ends in `agg` with `count`, `count_if`, `sum`, `min`, `max` or `mean`, or in a one-column `select`.
  Under `in`, it can also end in `select` or `group` without aggregates.
  The result equals running the sub-query per row ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Add duql correlated sub-queries that compare their own fields with the enclosing row in `where` terms `inner == ^.outer`, such as `where dur > (from data | where name == ^.name | agg { m = mean(dur) })`.
  The result equals running the sub-query per row, and an aggregate of no rows (`count()` 0) applies to a key without rows.
  `^.` in a range term, in a stage other than `where`, or after `take`, `sort` and similar stages is a compile error that points to `lookup ... asof` and `overlap` ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Run Python conversions of every column type natively in every build, with no pyarrow.
  `to_list`, `s[i]`, `to_numpy`, `to_pandas` and `to_polars` give `date`, `time`, `datetime` (zone-aware with `zoneinfo`), `timedelta`, `Decimal`, `float16`, fixed-size binary, fixed-size list and map values, or `datetime64`, `timedelta64` and object arrays.
  `Series.from_list` reads those Python types and takes a native `DType`.
  `Series.from_numpy` reads `datetime64`, `timedelta64`, `float16`, bool, text, bytes, object, strided and masked arrays.
  `from_pandas`, `from_polars` and `DataFrame.from_dict` read pandas and polars columns through NumPy, and only an Arrow-backed pandas column needs pyarrow.
  Lists and dicts in `Series.from_list`, and polars list and struct columns, import as list and struct columns ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Stop `astype`, `str.get`, `str.get_dummies`, `dt.isocalendar`, `compare`, `memory_usage` and `iter_rows` from calling `to_arrow` ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Make a build with `DFTRACER_UTILS_ENABLE_ARROW=OFF` read and write the lookup cache, the stored row sets and the aggregation tier.
  Arrow IPC remains for the Arrow exports ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Add a partitioned hash join on disk for a `LazyFrame` join whose right side outgrows the plan's memory budget, for every kind but `cross`.
  The rows and columns equal the in-memory join's, in no particular order.
  The spill format keeps a column's JSON flag, time unit and time zone and holds list, large list, fixed-size list, map and struct columns ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Let the trace server and viewer serve any JSON trace through its record schema.
  Timeline bounds, density, events, records without a time, the call tree, Analyze stats, histograms, the process tree and the column list read the time, duration and entity roles and the label field of a path schema, with times in microseconds.
  `/api/info` reports the schema and the field of each role.
  `/api/viz/proctree` gives each text entity a `label` and takes host and rank from the `hosts` and `ranks` row sets of the source.
  The lane, name and Analyze filters of the viewer name the fields of the schema ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let counter export (`sink_counters`) take the entity and lane group columns of a path schema as the counter `pid` and `tid`.
  A text value becomes a stable 31-bit id (`index::entity_id`) and stays in `args` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the record schema roles `lane` (the thread within an entity, dftracer `tid`) and `name` (the event name).
  Entity, lane and name fields are `int` or `string`, and a text entity or lane becomes a stable 31-bit id (`index::entity_id`).
  `call_tree`, `flamegraph`, `containment` and the View group keys `pid`, `tid` and `name` read these roles of any schema.
  A dftracer trace read through a schema with the same roles gives the same call tree and flamegraph ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let the aggregation tier build for files of any record schema.
  It answers the group keys `name`, `pid` and `tid` of a path schema and aggregates of its duration and time fields, with the same result as the scan.
  It answers only when each role field is required, the name is a `string`, the others are `int`, times are in `us`, the source of the schema has no `data` row set, and the index shows each role in every record and never negative.
  Any other plan scans ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let the View group keys `fhash` and `hhash` group the records of a path schema on the fields of those names.
  `file_path`, `file_name`, `host_name` and `rank` relabel them through the `files` (`fhash`, `path`), `hosts` (`hhash`, `name`) and `ranks` (`pid`, `rank`) row sets of the source.
  A schema whose source defines no such row set raises an error that names the row set and the schema ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let a time role be a `string` field of ISO-8601 times such as `2024-01-02T03:04:05.123Z`, read as microseconds since the epoch ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `lines_invalid` and `values_unconverted` to `ExportStats`, the Python stats dict and the stats frame.
  `lines_invalid` counts the lines that do not parse.
  `values_unconverted` counts the declared-field values that do not convert to the type of the field ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `dftracer_view` print the skipped line count ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the duration units `m`, `h` and `d` to duql ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `lookup s on k [== c], ... overlap [into m]`.
  Each row matches the rows of `s` with equal keys whose interval `[time, time + duration)` overlaps its own.
  Each match is an output row, in the row order of `s`, and a row with no match stays once with nulls.
  With `into m`, every row stays once with the list `m` of its matches ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `lookup s on k [== c], ... asof t [== c2] [backward | forward | nearest] [within n]`.
  Every row stays once, in order, with the columns of `s` (but its keys and its time column) from the row of `s` nearest in time among those with equal keys, as pandas `merge_asof` gives them.
  Among rows of `s` with equal times, `backward` and `nearest` take the last and `forward` takes the first.
  A null or missing key or time, or no candidate, gives nulls.
  `within n` drops a farther match.
  It does not combine with `into` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `session [k1, k2] gap d [max m] [as name]`.
  It gives the 1-based session of each row within its key, in time-role order.
  A session splits after more than `d` idle after the latest end (time plus duration) or more than `m` from the start of the session.
  A row without a time has a null session.
  Durations are in the time unit of the schema, so it works on any record schema ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make a duql pipeline scan only the fields its stages name when a later stage sets the output columns (`group`, `agg`, `select`, `pivot`, keyed `distinct`) and the index lists them all ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add JSON columns.
  A field whose records hold text in some and numbers or bools in others is a JSON column, and so is a declared `json` field.
  Rows and trace-plan group keys keep the type of each value, so `3` and `"3"` stay apart.
  JSON output writes the values as they are, `column_info()` reports `"json"` and Arrow carries it as the `arrow.json` extension.
  Python's `Series.is_json` names it and `to_list()` parses it.
  The C ABI adds `dftu_series_is_json` and `dftu_series_mark_json`, and C++ adds `Series::is_json`, `Series::as_json` and `DataType::json` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql sources.
  A record schema carries a `source` of named row sets (`name = pipeline;`) and macros, declared as the YAML or JSON key `source`, the C++ `static constexpr std::string_view SOURCE` or the Python class attribute `source`.
  `extends` merges the members of the parent by name.
  `from <row set>` reads one, `from data` (or no `from`) reads the `data` of the source and `from all` reads every record.
  The dftracer source declares `data` (every record but `ph` `M`), `files` (`fhash`, `path`), `hosts` (`hhash`, `name`), `strings` (`shash`, `value`) and `ranks` (`pid`, `rank`).
  Genesis adds `runs` (`run`, `app`, `system`, `unique_input`, `nodes`, `ppn`, `papi_set`, `method`, `sketch_accuracy`, `leaf`) ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add arrows that read a source row set, such as `fhash -> files.path`, `hhash -> hosts.name`, `exec_hash -> strings(shash).value` and `run -> runs.app`.
  Arrows work in filters, `select`, `derive` and `group`.
  A filter such as `where fhash -> files.path like "%/scratch/%"` is pushed down as `fhash in (keys of files)` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Store a row set that is a `where` and at most one `select` of record paths per file in the index extension `core.rowset`.
  Reading it decodes no trace and cells keep their JSON type.
  `explain_duql` shows `stored in the index` for these row sets.
  Other row sets run as lookup sides when the query executes ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add C++ `Indexer::rowset(name)` and Python `Indexer.rowset(name)`, which return the stored rows as a DataFrame ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql macros with `def name(a, b) = expr;` in a query, in a source or in `.duql` files on `$DFTRACER_DUQL_PATH`.
  `$DFTRACER_DUQL_PATH` is a colon-separated list of files or directories.
  Calls expand by position before planning, so a macro in a filter still pushes down.
  A query macro hides a source macro, which hides a path file macro.
  A cycle, a wrong number of arguments, a built-in function's name and a duplicate are compile errors ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `--duql-path PATH` (repeatable) to `dftracer_view`, `dftracer_stats` and `dftracer_comparator` to load more macro files ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `View::all()`, Python `TraceViewer.all()`, C ABI `dftu_view_all(const dftu_view*)` and `dftracer_view --all`.
  They read every record, metadata included, as rows that filters and aggregations see ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `GET /api/rowset?name=<row set>&key=<key column>&value=<value column>&keys=<k1,k2,...>`.
  It returns `{"names":{"<key>":"<value>"}}` and a key with no row is absent.
  The web UI reads host and file names through it, and a density group-by on a hash field resolves through the row sets ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql row sets and joins on a View with `let name = pipeline;`, `from`, semi-joins and anti-joins `k [not] in (from ...)` with tuple keys, arrows `k -> s.path`, `k -> s(id).path` and chains, scalar sub-queries, `lookup s on k [== c] [into m]` and `union (from ...)`.
  `from` takes `let` names, `all`, `data`, files, parameters and `from a, b`.
  Keys match by duql value.
  In a `where`, an arrow holds when any matching row makes it hold.
  Elsewhere, different values for one key are an error that names the row set and the key.
  Row sets run when the query executes, never in `duql()` or `explain` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Push a semi-join or an arrow comparison at the top of the leading duql `where` down as `k in {keys}` in the scan filter.
  Bloom and min/max pruning then skip chunks, and results are the same without it ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Cap lookup sides with `DUQL_LOOKUP_MAX_ROWS` (default 1,000,000) and `DUQL_LOOKUP_MAX_BYTES` (default 256 MiB).
  A side over a cap fails the query and never truncates it ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add a lookup cache at `.dftindex-cache/lookups`.
  It stays within `DFTRACER_CACHE_MAX_BYTES`, which the rollup store shares ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `JoinHow::Lookup` and `JoinHow::Nest` for DataFrame and LazyFrame joins, with C ABI `DFTU_JOIN_LOOKUP` and `DFTU_JOIN_NEST` and Python `how="lookup"` and `how="nest"`.
  Every left row appears once and keys compare by value, so `1 == 1.0` and `"1" != 1`.
  `lookup` adds the columns of the matching right row and fails when rows that share a key differ.
  `nest` adds a list of every match.
  The duql `lookup` stage uses them, so a side over the same files shares the scan ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `var`, `std`, `quantile` and `histogram` to the duql `window` block.
  They give the partition's value, the same as the aggregate in a `group` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the duql aggregates `count_distinct(e)`, `collect(e)`, `arg_max(e, by)`, `arg_min(e, by)`, `sketch(e)`, `merge(s)` and `quantile(merge(s), q)`.
  `count_distinct` compares by value, so `1 == 1.0`.
  `collect` gives a list in input order and `arg_max` and `arg_min` take the first row on a tie.
  `sketch` and `merge` use DDSketches as base64 text, the form trace summaries store.
  A block with one of them runs as one streaming pass in input order that also folds the other aggregates of the block.
  `explain` prints `group fold over ...` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the duql functions `slice(a, i[, j])`, `flatten(a)`, `keys(o)`, `values(o)`, `parse_json(s)` and `split(s, sep)`.
  They work in scan filters and in every stage after the scan ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `group k1, k2 { a = f(...), ... }` and `agg { ... }` on a View.
  Keys are fields or named expressions.
  Each block entry is one aggregate call whose arguments are expressions.
  The aggregates are `count()`, `count(e)`, `count_if(c)`, `sum`, `min`, `max`, `mean`, `var`, `std`, `first`, `last`, `quantile(e, q)` (within 1% relative error), `histogram(e)` and the occupancy aggregates `busy`, `concurrency`, `utilization` and `active`.
  Nulls are skipped and a null key is its own group.
  Rows come sorted by key with null keys last.
  `agg` over no rows gives one row ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Run a duql `group` right after the scan stages, with string or integer field keys and numeric field inputs, in the View's trace aggregation.
  This path uses the index, rollups and occupancy clipped to buckets.
  Any other `group` runs as computed columns and a LazyFrame `group_by`, with the same rows.
  `explain` prints `group (trace plan)` or `group (frame plan)`.
  Occupancy aggregates need the trace plan and otherwise fail and name the stage that prevents it ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the duql stages `take n by k [sort s]`, `sample n [seed s]`, `sample p% [seed s]`, `time_range lo .. hi [overlap]`, `bucket d [fill]` and `call_tree`.
  `take n by k` gives the first `n` rows per key.
  `sample` gives the same rows for the same query, seed and files, with any number of workers.
  A leading `time_range` is the View's `time_range`.
  `bucket ... fill` stops at `DUQL_FILL_MAX_ROWS` rows (default 10000000) ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `LazyFrame::head_by(keys, n)`, which streams the first `n` rows of each key tuple in input order.
  It is also in `LazyOps`, C `dftu_lazyframe_head_by` and Python `LazyFrame.head_by(keys, n)` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `window [k1, k2] [sort s] { a = expr, ... }`.
  Each entry is an expression over the window functions `row_number`, `rank`, `dense_rank`, `lag`, `lead`, `running_sum` and `running_count`, and over `count`, `sum`, `min`, `max`, `mean`, `first` and `last` on the partition.
  Every row stays in input order.
  Partitions are the key tuple, ordered by `sort` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `expand p [as e] [with_index i] [keep_empty]`, which gives one row for each element of an array.
  Object elements become columns `e.<field>` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `pivot k [in [v1, v2]] { a = agg(...) }`, which turns the values of `k` into columns `a.<value>`.
  Without `in`, more than `DUQL_PIVOT_MAX_COLUMNS` values (default 1024) fail.
  No stage may follow `pivot` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql `unpivot a, b as key, value`, which gives one row for each listed field ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add the duql quantifiers `any(p, e)` and `all(p, e)` with `.`, `.name`, `.[0]` and `^.name`.
  They work in scan filters, in stages after the scan and in DataFrame and LazyFrame filters over List columns ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make fields that `expand` or a quantifier reads List columns after the scan.
  The element type comes from the index path catalog, such as `List<Struct>` for objects ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let `View::duql`, `dftu_view_duql`, `TraceViewer.duql` and `dftracer_view --duql` take the new stages ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add List and Struct column support to DataFrame concat ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql pipelines on a View with `where`, `select` (with `x = e` and `e as x`), `derive`, `drop`, `rename`, `distinct`, `sort` (`-key`, `nulls first`), `take` and `skip`.
  Parameters are `$name`, counts included.
  Leading `where` stages filter the scan and a leading field `select` is its projection.
  The other stages run on the scanned columns.
  A pipeline with stages after the scan reads rows in file order, then line order, with any number of workers.
  Workers still decode in parallel and `take n` stops the scan once the first rows are out ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add C++ `View::duql(text, params)` and `View::explain_duql`, C `dftu_view_duql` and `dftu_view_explain_duql`, Python `TraceViewer.duql(text, **params)` and `TraceViewer.explain_duql` and `dftracer_view --duql` with `--param name=value` and `--explain` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add durations such as `250ms` and `10us`, and the functions `as_time`, `to_seconds` and `bin`, to duql.
  They use the unit of the time and duration roles of the record schema ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Run duql expressions after the scan and in DataFrame masks on column kernels.
  New expression nodes give them the duql meaning.
  They are `expr_arith`, `expr_neg`, `expr_coalesce`, `expr_extreme`, `expr_concat`, `expr_round`, `expr_log`, `expr_pow`, `expr_str_substr`, `expr_convert`, `expr_list_len`, `expr_list_get`, `expr_list_sum`, `expr_list_contains`, `expr_str_pattern` and `expr_str_extract`, with their C ABI entries.
  They add string, bool and null literals and comparison of two columns.
  Arithmetic is checked, so overflow and a zero divisor give null, `//` floors and `%` takes the sign of the divisor ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let DataFrame masks take pattern conditions and ordered comparisons of string and bool columns ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add `like ... escape "c"` and the function `extract(s, regex[, group])` to duql ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add expressions to duql filters.
  They are arithmetic (`+ - * / // %`, where `/` gives a double and `//` floors), comparisons between fields, `between`, `is [not] null`, `is [not] missing`, `??`, `in` lists of expressions, negative indexes such as `tags[-1]` and the functions `exists`, `coalesce`, `if`, `case`, `abs`, `floor`, `ceil`, `round`, `min`, `max`, `log`, `exp`, `pow`, `len`, `concat`, `lower`, `upper`, `trim`, `starts_with`, `ends_with`, `contains`, `substr`, `replace`, `first`, `last`, `sum`, `json`, `type`, `int`, `float` and `string`.
  Every path (View, readers, folds and DataFrame masks) gives the same result.
  A condition with an expression is checked per record and never skips chunks, except `exists(path)`, which skips a file whose catalog lacks the path.
  `x == null` and `null` in an `in` list are compile errors ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add plugin index extensions through the new `DFTU_SVC_INDEX` service in `plugins/abi/index.h`.
  While the plugin set is loaded, index builds store the bytes its builder returns for each chunk and file.
  Queries skip the chunks it rules out for a filter leaf.
  Data of an extension that is not loaded, or of another version, prunes nothing ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `Indexer::manifest`, `explain`, `rebuild_extension` and `drop_extension` name plugin extensions ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `--plugin` to `dftracer_index` to load plugins for a build ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add C++ `trace::views::rows<T>(view)`, which decodes each selected record straight into a schema class `T` on the scan workers.
  It is about twice as fast as `collect()` followed by reading the same columns ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add Python `TraceViewer.rows(cls)`, which yields schema class instances.
  Schema classes take their fields as keywords ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add an overload of `ViewSession::fold` without a predicate that folds the selection of the base view ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add a built-in `genesis` record schema for `dftracer_genesis_gen_dist` output.
  Its source adds the row set `runs` over the `RUN` lines.
  Keys such as `run -> runs.app` and `run -> runs.papi_set` work in filters, `select`, `derive` and `group`.
  Numeric keys stay numbers, such as `run -> runs.nodes == 4` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add array membership filters such as `any(tags) == "gpu"`.
  The filter holds when any element of the array at `tags` does.
  It works with every field operator (`==`, `<`, `in`, `not in`, `like`, `~` and others) and inside `and`, `or` and `not`.
  The index skips chunks through the evidence it keeps for each array position, and the line pre-filter stays exact.
  C++ `Field("tags").any()`, the C ABI builders with the field `any(tags)` and Python `F.tags.any()` build it ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add user record schemas that declare the typed fields of a format.
  A field has a type (`bool`, `int`, `float`, `string` or `json`) and a path.
  A field can also have `optional`, a `time`, `duration` or `entity` role with a unit, and `always_index`.
  A schema can also set `extends`, `index.path_budget` and `source`.
  Declare a schema as a YAML or JSON spec, as a Python class (`class A(dftracer.utils.schemas.RecordSchema, id="a")` with annotated fields and `field(...)`) or as a C++ class (`index::Field<T, "path", options...>` members in `index/schema_class.h`).
  Specs load from `$DFTRACER_SCHEMA_PATH` and from `<index_dir>/schemas/` when an index is opened.
  You can also register them with Python `dftracer.utils.schemas`, C++ `index::register_schema` and `load_schemas`, or C `dftu_schema_register`, `dftu_schema_load`, `dftu_schema_list` and `dftu_schema_detect`.
  Detection considers every registered schema.
  The always-indexed fields and the path budget of a schema shape the index of its files.
  Editing a schema rebuilds those indexes.
  Fields read as their declared types.
  `json` fields read as canonical JSON text, and filters compare them with a string literal.
  `time_range`, `time_bucket`, the occupancy aggregates, `call_tree`, `flamegraph` and containment work on any schema that binds time and duration.
  They convert units to microseconds, and `time_range` prunes through the index of the time field.
  A viewer can read files as another schema with `TraceViewer(paths, record_schema=...)`, `View::record_schema`, `dftu_view_record_schema` or `dftracer_view --schema`.
  Opening an index whose file records a schema that is not registered is an error.
  The error names the id and where schemas load from ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `schemas.explain(path)` (C++ `explain_file_schema`, C `dftu_schema_explain`) to report the detection share of each schema for a file ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `TraceViewer.schema_tree()` (C++ `View::schema_tree`, C `dftu_view_schema_tree`) to list the paths the index holds with their types, counts and declared fields.
  It does not scan records ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add a "no time" tab to the trace viewer for records written with `ts` 0, such as CUDA activity, events and aggregated records.
  The status bar counts these records, and the tab pages through them, longest first.
  The timeline cannot place them.
  The tab reads `GET /api/viz/untimed` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Let views read JSON lines files of no trace format (the `generic` schema).
  Collect, filter, `select`, sorting, `group_by` on any path and every aggregate work on their records.
  Columns are named by exact path, such as `op` and `io.off`, and the schema comes from the path catalog.
  The View decodes only the paths a query reads.
  On a 1M-record trace, a full collect takes 1.2 times as long as the dftracer decoder and a grouped aggregate is as fast.
  Trace operations (`time_range`, `time_bucket`, occupancy, `call_tree` and `flamegraph`) raise an error on the `generic` schema, because it binds no time role.
  The dftracer-only group keys raise an error too.
  A View over files of different schemas raises an error ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Let `View::from_directory`, the `Indexer` and `dftracer_index --directory` find `.jsonl.gz` and `.ndjson.gz` files ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `dft.metadata`, an index extension that a build makes with the pruning tier.
  Per chunk, it holds the number of metadata (`ph="M"`) records and of context records among them, such as thread and process names, `PR` and `CM`.
  It also holds the distinct names and field paths of these records.
  Read it with `IndexDatabase::chunk_metadata`.
  An existing index gains it on its next build ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `agg_clip_occupancy` to bound the occupancy of an `AggState` by a window and time buckets before `agg_finalize` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add record schemas (`dftracer/utils/index/record_schema.h`) that detect the record format of each file from its first lines and record it with the file (`core.profile`, `IndexedFile::schema`).
  A `generic` file, which is any gzip of JSON lines, is indexed by exact path.
  Its path catalog and automatic zone maps, blooms and value counts cover top-level and nested fields.
  A filter such as `op == "read"` skips chunks.
  Set the schema with `IndexerOptions::schema`, `schema` in `dftu_indexer_options` and `dftu_indexer_file`, Python `Indexer(schema=...)` or `dftracer_index --schema` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `memory_budget` for index builds (`IndexerOptions::memory_budget`, `memory_budget` in `dftu_indexer_options`, Python `Indexer(memory_budget=...)` and `dftracer_index --memory-budget`).
  It has the meaning that `View::memory_budget` has.
  Under a budget, a build works on fewer large files at once.
  A file whose evidence outgrows its share spills finished chunks to sorted runs and commits them with the rest in one atomic ingest.
  The index holds the same data.
  On a 46 MB trace with 4000 args paths, a 256 MB budget keeps the build near 284 MB peak RSS ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add a path catalog (`core.catalog`) that lists, for every indexed file, each JSON path its data records hold.
  Paths are as written, such as `args.io.off` and `args.hosts.1`.
  Each path has a type (int, uint, double, bool, string or mixed) and a non-null record count.
  Read it with `IndexDatabase::catalog` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `Indexer::manifest` and `Indexer::explain`.
  `manifest` shows which extensions each indexed file has.
  `explain` shows, for a query, which chunks it reads and which chunks each pruning extension rules out on its own ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `rebuild_extension` and `drop_extension` to rewrite or remove one of `zonemap`, `bloom`, `counts`, `postings` or `dft.stats` without touching the others ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `IndexerOptions::extensions` to choose which pruning extensions a build makes ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add the manifest, explain, rebuild, drop and extension calls to the indexer C ABI and Python.
  `dftu_indexer_manifest` and `dftu_indexer_explain` return owned JSON strings that you free with `dftu_indexer_string_free`.
  Also add `dftu_indexer_rebuild_extension`, `dftu_indexer_drop_extension` and `extensions` in `dftu_indexer_options`.
  In Python, use `Indexer.manifest()`, `explain()`, `rebuild_extension()`, `drop_extension()` and `extensions=` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `dftracer_info --query detailed` print the extensions of each file ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Let the indexer and the reader handle a trace whose last gzip member was cut short, such as by a job killed while writing it.
  They index and read the trace up to its last complete line, and do not lose that member.
  The cut member is decoded with zlib-ng when libdeflate cannot, and only when the data ran out and is not corrupt.
  `Indexer` status and files, the indexer C ABI and the Python `IndexStatus` report such traces as truncated.
  `dftracer_validate` still reports a cut trace as invalid.
  `dftracer_genesis_gen_dist` still skips a run whose trace is cut ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `Indexer` (`dftracer/utils/index/indexer.h`), the public C++ entry point to build an index.
  Call `Indexer::open` over trace files and directories.
  Then call `status`, `build`, `rebuild` and `files`.
  `build` handles only files that miss a requested tier or changed since indexing.
  Each call has a blocking form and an async form that runs in the `CoroScope` of the caller.
  A build that cannot read a trace throws an error that names it ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add the indexer C ABI (`dftracer/utils/index/abi.h`).
  It has `dftu_indexer_open`, `dftu_indexer_status`, `dftu_indexer_build`, `dftu_indexer_rebuild`, `dftu_indexer_files` with a file-list handle, and `dftu_indexer_free`.
  The calls return `DFTU_RESULT` value-or-error results ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `dftracer_genesis_gen_dist` to turn genesis sweep traces (matrix and tioga layouts) into one `.pfw.gz`.
  The file has a `ph:4` `RUN` line per run and a `ph:3` record per call path.
  Each record holds `dur` and attributed `counters` distributions (min, max, sum, avg and p25 to p99).
  The tool skips incomplete or unreadable runs with a message and a non-zero exit status ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add `BasicDDSketch<BINS>` to let a caller pick the bin count.
  `DDSketch` stays the 128-bin sketch ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Let the Python `Series`, `DataFrame` and `LazyFrame` classes, `op_run` and the columnar expression engine work in a build with `DFTRACER_UTILS_ENABLE_ARROW=OFF`.
  Only the Arrow interop is compiled out.
  This covers `__arrow_c_array__`, `__arrow_c_stream__`, `to_ipc`, `_series_from_arrow`, `_dataframe_from_arrow`, `LazyFrame.stream` and the Arrow record-batch iterators.
  Their wrappers raise a `RuntimeError` that names the missing Arrow support.
  The default build is unchanged ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Add an optional end column and a maximum span to the `sessionize` window function.
  In Python, use `("sessionize", time_col, gap, out[, end_col[, span]])`.
  In C, use `dftu_window_spec.param.session` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let directory discovery list `.pfw`, `.jsonl` and `.ndjson` files, plain or gzip.
  This applies to `View::from_directory`, `Indexer`, `TraceIndex` (`dftracer_server`), the plugin host, the Python dask and dfanalyzer helpers and every CLI.
  Use `trace_file_patterns()` in C++ and `TRACE_FILE_PATTERNS` in the extension module.
  Recursive scans skip `schemas/` directories ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Make `dftracer_view -d` scan subdirectories, as `View::from_directory` does ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let record schema detection read the lines of JSON array traces (`[` and a trailing comma) ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add duql query syntax.
  Query strings are now parsed by the duql front end, the first stage of the duql query language.
  Filters keep their meaning.
  New syntax includes backtick-quoted keys (``a.`b.c`.d``), `#` comments, `$name` parameters bound through `duql::parse(text, params)` and an optional `duql 1` first line.
  Errors now name the line and column.
  Constructs that the engine does not evaluate yet, such as arithmetic, functions, pipelines and lookups, parse and fail with an error that names the stage that adds them.
  See the duql syntax reference ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Add flattened nested and array args to the default trace columns, such as `args.dur.p99` and `args.hosts.1`.
  Bool args read as 0/1 integers.
  `columns`, `schema` and `column_info` list every array element and every dotted arg name ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Let a short name such as `dur.p99` resolve to args in `select`, `group_by` and `call_tree` fields, as it does in `filter` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Add index features to records decoded by path.
  An indexed export (`sink_trace` with `build_index`, and row materialized views) decodes them by their schema and records it.
  A first query on an unindexed file builds its index in the same pass.
  A whole scan builds a missing pruning tier along the way ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Let `LazyFrame.stream` work in a build without Arrow.
  It yields native DataFrame chunks ([cd9b4e3](https://github.com/llnl-asr/dftracer-utils/commit/cd9b4e3d6d2d)).
- Make `jit_op.run_op_array` work without pyarrow ([cd9b4e3](https://github.com/llnl-asr/dftracer-utils/commit/cd9b4e3d6d2d)).
- Allow a negative duration such as `-1ms` in duql ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Add `PLUGIN_ABI_VERSION` to the Python extension module, the plugin ABI version this build accepts ([c329c07](https://github.com/llnl-asr/dftracer-utils/commit/c329c07237e7)).
- Add a "Copy for agent" item to the copy menu of each docs page.
  It copies a short prompt with the page title, the address of the page's Markdown and the address of `llms.txt`, for an agent that can fetch web pages ([98bba74](https://github.com/llnl-asr/dftracer-utils/commit/98bba745c6d5)).

### Removed

- Remove the Arrow sort-merge C++ functions `join`, `asof_join`, `interval_join`, `window` and `gap_fill` ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Remove `resolved.fpath`, `resolved.cwd`, `resolved.hostname`, `resolved.host`, `resolved.exec`, `resolved.cmd` and the `r.` short form.
  Write arrows such as `fhash -> files.path` instead ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove `IndexDatabase::write_agg_global_config`, `write_agg_file_markers` and `write_aggregation_tracker` from C++ ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove `open_with_merge_operator` and `open_read_only_with_merge_operator` from `EventAggregator` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove `resolved.<key>.<field>` names, the Python `resolved()` helper, the C++ builders `duql::resolved` and `dataframe::field::resolved`, and `GroupKey::resolved` and `DFTU_GROUP_KEY_RESOLVED`.
  A `resolved.` name now fails with an error that names the arrow to write.
  Use `fhash -> files.path` for `resolved.fhash.path`, `cwd -> files(fhash).path` for `resolved.cwd.path` and `hhash -> hosts.name` for `resolved.hhash.name`.
  Use `exec_hash -> strings(shash).value` for `resolved.exec_hash.value` and `run -> runs.app` for `resolved.run.app` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove the `dictionaries:` key of the record schema, `RecordSchema::dictionaries`, the `core.dict` index extension, the `IndexDatabase::dict_*` API and Python `Indexer.get_dictionary`.
  The `dictionaries:` key is now an unknown key error.
  Declare `source:` row sets instead, use `core.rowset` for `core.dict` and use `Indexer.rowset(name)` for `Indexer.get_dictionary(name, field)` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove `View::metadata(bool)`, `View::emit_all_metadata(bool)`, Python `TraceViewer.metadata(bool)`, `dftracer_view --no-metadata` and the re-emission of hash metadata and context records beside data events.
  Drop `.metadata(False)` and `--no-metadata`, because this is now the default.
  Use `.all()` for `.metadata(True)` and `emit_all_metadata`, or `--all` to emit every record ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove `GET /api/resolve`.
  Use `/api/rowset?name=files&key=fhash&value=path&keys=H` for `/api/resolve?type=file&hash=H` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove `dftracer_view --stream`.
  The flag had no effect, because matching events always stream to the output.
  Drop the flag from scripts ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Remove the `dft.hash` index extension (`IndexExtension::HASH`) and its API.
  This covers `IndexDatabase::HashType`, `query_hash_table`, `resolve_hash`, `lookup_hash`, `count_hash_entries`, `resolve_name_to_hashes`, Python `Indexer.get_hash_table` and `count_hash_entries` and the `proc` type of `/api/resolve`.
  Use the source row sets instead ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove `IndexDatabase::query_all_column_data_types` and the stored column records it read.
  `query_all_column_types` now answers from the path catalog ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove C++ `IndexExtension::INDEX` and the reads of its payloads.
  This covers `query_chunk_bloom_filters*`, `query_file_bloom_filter*`, `query_chunk_dimension_stats*`, `query_index_dimensions`, `query_time_bounds`, `query_merged_statistics_batch`, `file_has_name` and `query_name_chunk_postings`.
  Use `extension_paths`, `path_granules`, `path_file_value` and the postings reads ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove `BloomFilterCache`, its `View::from_files` argument, `ViewPlannerInput::with_bloom_cache` and `ChunkPrunerInput::cache`.
  The cache was written and never read ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove the `@auto` dimension marker.
  Auto fields are now part of the tier parameters ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove C++ `IndexBatchSink` and its `insert_*` methods.
  Use `IndexWrite` and `store::records` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove the C++ file capability bits in `index_file_entry_capability.h`.
  Use `IndexDatabase::extension_state` and `extension_current` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove C++ `has_bloom_data`, the root summaries (`rebuild_root_summaries`, `query_root_*`), the global name dictionary (`query_name_id`, `query_name_by_id`, `query_name_file_postings`) and the per-file category and name count records.
  Use `file_has_name` in place of the name dictionary ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Replace C++ `register_files` with `assign_file_ids`, which writes no record ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Replace the `SstArtifactRegistry` per-family accessors with `files(Family)` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Rename `SCHEMA_VERSION`, `schema_outdated` and `Freshness::SchemaOutdated` to `FORMAT_VERSION`, `format_outdated` and `Freshness::FormatOutdated` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove Python `IndexDatabase.register_files`, `IndexDatabase.rebuild_root_summaries` and the `rebuild_root_summaries` argument of `dftracer.utils.dask.distributed_index` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Change `IndexDatabase.find_stale_files` to report `format_outdated`.
  SST artifact dicts now use `<family>_sst` keys ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove `dftracer_index --rebuild-summaries` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove the C++ sub-chunk skipping API.
  This covers `index::plan::enumerate_work_items`, `index/plan/chunk_geometry.h`, `index/plan/sub_chunk_prune.h`, `ReadConfig::sub_event_counts` and `sub_keep`, `ChunkIndexerConfig::sub_chunk_events`, `ChunkStatistics::sub_zonemaps` and `ViewPlannerInput::with_cached_chunks`.
  No production path used them ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove the C ABI per-file gzip checkpoint API (`dftu_indexer_create`, `_build`, `_need_rebuild`, `_exists`, `_get_max_bytes`, `_get_num_lines`, `_destroy`), the C reader API (`dftu_reader_*`) and the C reader stream API (`dftu_reader_stream*`, `dftu_stream_config_t`).
  Build indexes with `dftu_indexer_*` and read events through the View C ABI in `dftracer/utils/trace/views/abi.h`.
  Raw byte-range and line-range reads are C++ only ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Stop installing the C++ index build headers `index/build/resolve_and_build.h`, `index/build/batch_builder.h`, `index/build/resolver.h`, `index/store/shard_manifest.h`, `index/store/index_batch_sink.h` and `index/schemas/dft/agg/aggregation_drain.h`.
  Use `Indexer` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Remove the previous plugin ABI.
  A plugin built against it does not load.
  Rebuild the plugin against `dftracer/utils/plugins/abi/plugin.h` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a), [60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
### Fixed

- Fix a stack overflow in builds that GCC compiles without sibling-call optimization (`-O0`, `-Og`, `-O1`, as in a Debug build): every awaited `CoroTask` or generator value that was ready at once added stack frames, so a spilling `join`, `asof` or `interval` crashed a worker thread after about 50,000 of them. An awaiter now runs the child on its own stack and continues without a new frame when the child is done.
- Fix the order of a sort of a float column that holds both NaN and nulls: a column with nulls was sorted with `<`, which is not an order once a NaN is among the values, so `sort_by`, `sort_by_multi`, `argsort` and the spilled sort returned the rows out of order. Such a column now sorts its values, then its NaN, then its nulls. `unique` of a float column with nulls now returns its NaN rows last, as it already did for a column without nulls.
- Order Date, Timestamp, Time, Duration, Binary, fixed-size binary, Decimal and Float16 keys correctly in a spilled sort: the merge read every key that was not an integer, float or string as 0.0, so a `sort_by` on a Timestamp or Date column under a small budget came out in the wrong order. Float keys in the merge now order as the sort kernels do (every NaN equal and above infinity), where NaN used to compare equal to every value.
- Raise an error when a spill run cannot be written (a full disk), when a run or part file is missing, or when it is cut short, in the run writer, the aggregate run writer and reader, and the group-by part files. Before, an unchecked stream write or a short read ended the run early and rows were lost without an error.
- Create the spill directory of a plugin state under `spill_dir()`, so it follows `DFTRACER_UTILS_SPILL_DIR` and avoids RAM-backed temp directories like every other spill, instead of the system temp directory.
- Name the input frame of a lazy frame op from its cursor when the plan has no static schema: `compare_agg` (and any `frame_op`) over a plan whose columns are only known after it runs failed with "the op returned no frame" because the collected frame had no column names.
- Free the overflow slot of the Arrow IPC writer buffer pool and release a pooled slot when a batch fails to encode: a batch written while every pooled slot was busy leaked its slot, and an encode error kept its pooled slot busy for good. `BufferPool::acquire` now returns a `Lease` that releases or frees the slot on every path out.


- Bound the memory of several lazy ops under a memory budget. A new test (`dataframe/test_op_peak_memory`) runs each op over 4M generated rows at a 16 MiB budget and reads its peak memory: `sort_by_multi` goes from 500 to 35 MiB (it spooled its input and then loaded all of it into one frame; it now uses the external merge sort of `sort_by`, which takes several keys), `reverse` from 498 to 35 MiB (a row index, an external sort and a drop of the index), `take` from 285 to 1 MiB (one pass that keeps only the wanted rows; an index past the end now raises), `group_by` from 239 to 44 MiB (the number of morsels accumulated at once now follows the budget, and `agg_approx_bytes` counts the group index and allocated capacity), `is_duplicated` and `is_unique` from 890 to about 65 MiB (past the budget they sort the rows by key on disk instead of keeping a count per distinct row), and `sort_by` from 86 to 40 MiB (runs are cut at a quarter of the budget). A spilled `group_by` now cuts its state into hash parts and merges each part in bulk, instead of merging sorted runs one group at a time: with 4M groups it takes 5.0 s instead of 14.0 s at a 16 MiB budget and 2.6 s instead of 11.5 s at 1 GB (its output then comes in part order, not key order). `unique`, `drop_duplicates` and `unique_by` go from 268 to 16 MiB and from 4.2 s to 1.2 s past the budget (`head_by` from 207 to 17 MiB, and it now spills too): the rest of the input is spooled once, only the row ids and keys are hash-partitioned to disk, and the spool is replayed with just the surviving rows, where each row was written three times and merged back before; below the budget they take the same time as before.

- Bound the memory of `dftracer_genesis_gen_dist` with the new `--memory-budget` (default a third of the available memory, cgroup aware). It was killed by the out-of-memory killer at 239 GB on a laghos sweep, because it read every call of a run into memory, copied them again to merge the files, and admitted runs by a size guessed from their compressed bytes (about 350 bytes of memory per call). It now keeps only a bounded part of each run in memory, spills the rest of the calls to a file and sorts them out of core, and gives each run at once an equal share of the budget. The output is identical to before for every input. On an 8M-call run in 16 files, peak memory is 1.2 GB instead of 1.65 GB with the default budget in the same 1.0 s, 348 MiB with `--memory-budget 256MB` in 1.1 s and 102 MiB with `--memory-budget 64MB` in 1.6 s. A run whose counter series or path records exceed their share is skipped with a message naming the share. The spill directory is now chosen automatically: `DFTRACER_UTILS_SPILL_DIR` when set, else the writable node-local disk mount with the most free space (never RAM-backed or network), else a per-user directory under `/var/tmp` or `~/.cache` on a local disk, else the system temp directory with a warning when that is RAM-backed. The lazy engine uses the same `spill_dir()`, which moved to the core library and reports its failure as an error value.

- Accept `query` wherever `duql` filters a view: `View::query` and `View::explain_query`, Python `TraceViewer.query`, `TraceViewer.explain_query` and `DaskTraceViewer.query`, and `--query` next to `--duql` in the CLI tools.
- Add `dftu_view_phase` to the trace view C ABI; `dftu_view_all` reads every record like `phase("any")`.
- Make `phase("any")` read every record, metadata included, and remove `View::all()`, Python `TraceViewer.all()` and `dftracer_view --all`: a view with no phase reads the record schema's `data` row set, `phase("any")` (`dftracer_view --phase any`) reads all records.
- Give every branch of a fused session the rows it returns alone when it reads metadata records (`phase("metadata")`, or a view from `all()`). A row collect next to another branch returned no metadata rows (for example `name == "EH"` next to `cat == "PROV"` returned 0 instead of 673), a metadata branch next to a default-phase view saw none, a group-by next to a row collect left the metadata records out of `all()`, and the raw lines of metadata records were dropped whenever an aggregation shared the scan.
- Print the shortest round-trip digits for a double or float in a build whose C++ library has no floating-point `std::to_chars` (a macOS wheel for a target before 13.3): such a build wrote `0.12345678910000001` for `0.1234567891` in every text output, from duql results to exports ([279aa01](https://github.com/llnl-asr/dftracer-utils/commit/279aa01fac91)).
- Sum rolling windows with Kahan compensation, as pandas does: a plain running sum drifted by the magnitude of the values it had added and removed, so `rolling(n).sum()` over a long series no longer rounded like a fresh sum of the window and differed from pandas in the last bits ([164f754](https://github.com/llnl-asr/dftracer-utils/commit/164f754bed63)).
- Free every frame of a scan its consumer stopped early (`take`, a dropped cursor): the producer parked on a full channel or on the byte budget held its frames and morsels until the process ended, and a runtime shut down while it was unwinding destroyed queued frames and leaked the ones still awaiting them. The cursor now closes its channel as it releases the budget, a run loop runs what the finished task left runnable, and an executor lets runnable work and file reads still in flight finish before its workers stop ([288d8fe](https://github.com/llnl-asr/dftracer-utils/commit/288d8fe24184)).
- Fix memory corruption under GCC 12: an object built as a temporary inside a `co_await` expression (a `PruneRequest`, `ChunkPrunerInput`, `CoverageSet`, a `std::string{}` or a braced options struct) was destroyed twice, which crashed View queries, index pruning and the trace reader (ten View tests segfaulted and nine index and reader tests aborted). Every such argument is now a named local. All 343 tests pass under GCC 12 (`make docker-test-gcc12`), which now runs as the invoking user, so the tests that make a file unreadable work. Frame quantile checks allow 16 ulp, because GCC fuses the interpolation into one multiply-add ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Build and test on GCC 12, the compiler floor, again: the Docker images install `ragel` for Vectorscan (building it from the shared source cache failed in the container), `DataType::fixed_size` is `constexpr` (GCC 12 rejects the non-`constexpr` call from `byte_width`), the number-to-bool cast kernel stores mask bits with `StoreMaskBits` on at most 64 lanes (SVE has no `BitsFromMask`), genesis discovery builds its scanner input before `co_await` (a braced list inside a `co_await` expression crashes GCC 12), and the struct size budgets count `std::string` by its size (32 bytes in libstdc++, 24 in libc++) ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Write 1-based `first_line_num` in the member table of an indexed export (it was 0-based), matching the member table a normal index build writes for the same file ([490f04e](https://github.com/llnl-asr/dftracer-utils/commit/490f04e4e668)).
- Return every column a scan decodes from every lazy step on a trace whose index has no path catalog. `sort`, `take`, `derive`, `distinct`, windows, `fill_null`, `with_row_index`, explode and the left side of a join kept only the declared fields (3 of 33 columns of a genesis trace, 7 of 408 of a dftracer trace with counters); a field the query named, such as `count` in `sort -count`, was dropped as well, and an expression on an undeclared field (`derive d = count * 2`) gave nulls. Undeclared fields now travel in one internal struct column through every step that keeps rows, are typed from each batch, and become named columns at collect after the declared ones. A join gives the right side's undeclared columns the join suffix when the left side has undeclared columns too; a plugin step refuses such a plan with an error naming it. Concatenating struct columns now matches fields by name, and concatenating int and float columns by position widens to float ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Fix duql `drop` and `rename`, `LazyFrame.drop`, `LazyFrame.rename` with a mapping and `TraceViewer` on a trace whose index has no path catalog (any index built by a query), which kept only the declared fields: `drop v.p50` on a genesis trace returned 3 columns instead of 32. Drop and rename now act on every column a scan returns, through the new `LazyFrame.drop`/`rename_columns` ops (`dftu_lazyframe_drop`, `dftu_lazyframe_rename_columns`), with no index build ([b022374](https://github.com/llnl-asr/dftracer-utils/commit/b022374a0413)).
- Fix `TraceViewer(...)` and `View.from_files`, which ended the Python process with an abort instead of raising when the trace's index could not be checked, such as an index built with a schema that is not registered ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix `dftracer_replay`, which aborted on a pipeline error such as a trace that is not JSON; it now reports the error and exits with code 1 ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a selection over a dictionary column, or any column over a non-flat base other than a view, which made kernels that read it flat (such as `argsort`) fail with a null result ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a View over a trace that changed after it was indexed, which could return the old content's rows: a rewrite kept the file's old blooms, statistics, row sets and aggregation rows, so a query for the new values could return no rows. A View now indexes a changed file again through the resolver when it is created ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix an index rebuild while reading a trace, which deleted the whole index directory, including the records of other trace directories that share it through `--index-dir` ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix string kernels that build more than 2 GiB of output (`gather`, `where`, string transforms, `regex_replace`, `extract`, list transforms, duql text functions, string group keys and aggregates), which wrapped their 32-bit offsets; a gather and `where` now give a large-offset column and the others fail with an error ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a dictionary column whose dictionary holds a null value, whose rows read as valid in `null_mask`, `count` and `drop_nulls` ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix `Series::string_at`, which gave an empty string for a dictionary or selection column ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix the Arrow import of a sliced array, such as a pyarrow `slice()`, which read the rows and nulls from the start of the buffers ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix the Arrow export of a selection column with outer-fill rows, which wrote the index -1 ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix `to_ipc` of a dictionary column, which failed ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix `concat` of a JSON column that is not flat, which crashed ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a string column over 2 GiB built with `Series::strings`, which wrapped its offsets; it now throws `std::length_error` ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a query on a name a record holds at its top level, such as `id == 5`, which was pruned by the index evidence of an `args` key of the same name and could return no rows; a query on `args.<name>` now uses that evidence ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
- Fix a duql `bucket` whose width is not a whole number of microseconds, such as `bucket 1500ns`, which ran on the trace plan with a cut width and gave wrong buckets; it now runs on the frame plan ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a duql index into a column an earlier stage derived, such as `derive l = split(s, ",") | derive y = l[1]`, which read a missing record field and gave null ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix the partial view aggregation of `dfanalyzer` to return its columns in the order of `build_partial_meta`, so a Dask cluster run no longer rejects the partitions.
  `merge_view_partials` keeps the dtype of every partial column, including a column that is all null, and accepts a Dask frame ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `DaskFrame.reduce`, `group_by().agg()`, `shuffle`, `join` and `map_partitions` on a frame with no partitions, which raised a bare `IndexError`: each now raises a `ValueError` that names the operation and says the frame has no partitions ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a build over a corrupt aggregation tier, which failed with `aggregation tier write: Corruption: Merge operator failed`: it now clears the tier and aggregates every file again, once, and logs a warning that names the index directory. A direct read of a corrupt tier fails with an error that names the directory and says to delete it and build again ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a build that fell back to a full rebuild after failing to read the index, which kept the old aggregation tier rows and counted every event twice: it now clears the tier first ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix the Parquet round trip with pandas and Dask.
  `to_parquet` and `write_parquet` now write the pandas metadata block, so `pandas.read_parquet` restores `Int8` to `Int64`, `UInt8` to `UInt64`, `boolean` and `string` for every integer, bool and string column that holds a null; they read back as `float64` and `object` before.
  A column with no null reads as pandas reads it by default, and a float with nulls as `float64`.
  `concat` accepts `string` and `large_string` columns, alone or mixed (it failed with "column type 'large_string' is unsupported"), and so does the lazy concat; the result is `large_string` when any part is or when the bytes pass 32-bit offsets.
  The diagonal concat no longer turns a `string` / `large_string` mix into quoted JSON text.
  `from_parquet` drops Dask's `__null_dask_index__` column when the file's pandas metadata lists it as an index column; any other stored index stays a column.
  Unpivot of two `large_string` columns now concatenates them where it was refused ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a number on the left of `-` or `/` in a column expression: `lit(100) - col("x")`, `1 / col("x")` and the `//` and `%` built from them failed with "expr: scalar - / column has no kernel".
  The native compiler now broadcasts the scalar and runs the column kernel, and `Expr` gains the reflected division (`1 / expr`), which raised `TypeError`.
  Rebuild the extension to get the compiler part ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `a ** b` in the source tier of `apply` and in `eval` crashing with "the truth value of a column expression is ambiguous" when the exponent is a column; it raises `TranspileError` that says the exponent must be a constant ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `corr`, `covar_pop`, `covar_samp`, `regr_slope`, `regr_intercept` and `regr_r2` (and so `Series.corr`, `Series.cov`, `DataFrame.corr` and `DataFrame.cov`), which lost their digits when the values sit far from zero next to their spread.
  The group-by kept the raw sums `sum x`, `sum y`, `sum x^2`, `sum y^2` and `sum xy` and subtracted them at the end; over one million pairs of mean 1e9 and 5e8 and spread 1, `Series.corr` gave 0.0 and `group_by().agg(covar_samp)` gave -671.09, where pandas gives a correlation of 0.447672 and the covariance is 0.500467.
  A new accumulator keeps the central co-moments about a running mean pair (Welford updates and Chan merges, the same design as `std` and `var`), so any partitioning of the rows merges to the one-pass value.
  `Series.corr` and `DataFrame.corr` now give NaN for a column with no spread, as pandas does; the group-by `corr` aggregate keeps its readout of 0 for no spread or fewer than two pairs.
  The partial-aggregate blob (spill files and distributed partials within one run) changes layout in its co-moment block; no stored index changes ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `DataFrame.replace` returning the frame unchanged, with no error, when the replacement was null, such as `df.replace(float("inf"), pd.NA)` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `std`, `var`, `skew` and `kurt`, which lost their digits when the mean is large next to the spread.
  The engine kept raw power sums and rebuilt the variance as `sum x^2 - (sum x)^2 / n`; over one million values of mean 1e9 and standard deviation 1 `Series.std()` gave 0.0 and `group_by().agg(std)` gave 152.38, where the value is 1.000672.
  The shared accumulator now keeps central moments about a running mean held as a shift plus a small remainder (Welford updates, Chan/Pebay merges, and a corrected two-pass in the vector kernels), so every engine (`Series`, `DataFrame`, `group_by`, the View aggregation and the aggregation tier) matches a two-pass computation to a relative 1e-9 at any offset.
  `rolling_std` was already accurate; `ewm_std` now runs on values shifted by the first valid one.
  `DaskFrame` combines `var` and `std` from per-partition counts, means and sums of squared deviations, taken about a per-group reference so they stay accurate at any offset.
  `sumsq` is rebuilt from the central moments, so it equals the sum of squares to about 1e-15 relative, where the raw sum of small integers was exact.
  The stored aggregation tier now keeps central moments and the rollup cache layout tags change. Stored formats change freely until the first release, so delete a development index built before this change; no code reads an old development format.
  A single value still gives NaN and a constant column 0.
  The serialized partial-aggregate blob gains one field and is written and read by the same version within a run ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix an index whose aggregation tier carried another version number, which was re-aggregated on top of its old rows and so counted every event twice. The whole tier is now cleared and aggregated again once.
  A stored aggregation manifest of another version now marks the whole tier stale: it is cleared and rebuilt ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix the aggregated-trace writer's `_std` and the comparator's Cohen's d, which read the stored second moment as a raw sum of squares; and the writer's parser, which did not skip the stored third and fourth moments ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `set_union` over a list column, which gave `0` for every group.
  It now gives the union of the elements of the lists in the group, as if the lists had been exploded first; a null list, an empty list and a null element add nothing ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `sum`, `mean`, `min` and `max` of a bool column, which returned 0.
  A bool column counts `True` as 1 (`sum` is the number of true values, `min` and `max` are 0 or 1), so `(x > 2).sum()` counts the matches and agrees with the group-by sum ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `min` and `max` of a string column, which returned 0.
  They give the bytewise smallest and largest string, and the empty string when no value is valid ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix the count of a profile or system row that carries its count in `dft_cnt`.
  `dftu_cnt` still wins when a row has both ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `min`, `max` and `sum` of `offset` in a grouped `TraceViewer` query that the aggregation tier answers, which came back null.
  A group with no `offset` stays null ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a profile row at the first microsecond of an aggregation bucket landing in the bucket before, so the index and the standalone aggregator now agree.
  Indexes built before this change rebuild their aggregation on the next build ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix `asof` with direction `nearest` to take the last right row on an exact match among right rows with equal times, as pandas `merge_asof` does.
  Before, it took the first ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a diagonal concat of a column that is text in one part and a number or bool in another.
  It now gives a JSON column instead of turning the numbers into text ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix integers above the int64 range.
  They read exact as uint64, and wider ones read as their digit text, instead of dropping the line ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix doubles in group keys and text to print in shortest round-trip form, such as `2.5` instead of `2.500000` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix an expression whose value is a constant, such as `derive y = 1`.
  It is now a column of that value instead of an error ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix the time window so it skips a chunk whose start-time histogram has no event in the window, even when the chunk also holds events at `ts` 0 ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make an index build commit the data of a file together with a per-file manifest entry in one atomic write.
  Data without a current entry is never read, so an interrupted build leaves files unindexed instead of indexed but empty ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix `F("name") == 'a"b'` in the Python `F` filter builder, which matched nothing.
  The builder now writes `'a"b'`.
  A string that no duql literal can hold raises `ValueError` ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a duql duration that took the unit of the first timed field it found in any expression.
  The unit of a column now follows the value it holds through `derive`, `select`, `rename`, `group` and `agg`.
  `coalesce`, `if`, `case`, `min`, `max`, `abs` and `??` keep the unit of their operands ([35d2d10](https://github.com/llnl-asr/dftracer-utils/commit/35d2d100dbb8)).
- Fix a crash in a spilled join with a text column when the right partition held no rows ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Fix two scans of one trace in a process that could fail on the index database lock ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Fix two scans of an unindexed trace that could build its index at once.
  Before, a failed check could remove the whole shared `.dftindex` root with every other file's index and cache.
  Index checks and builds of one root now run one at a time.
  The rebuild of one file replaces only the records of that file ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Fix the JSON flag of a group key over a JSON column, which `group_by` now keeps on any plan.
  A correlated sub-query no longer needs a plain field key to keep the value type ([a8eaba8](https://github.com/llnl-asr/dftracer-utils/commit/a8eaba8671a1)).
- Fix a row set column whose type only the data gives, such as a dftracer `args.value`, that read as null.
  This happened through an arrow (`pid -> ranks.rank`), a `derive` or a `where`.
  The column now takes its type from the rows of the row set ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `from all` on a dftracer trace, which dropped the args of metadata records (`args.name`, `args.value`) from a `take` or collected frame ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a null group key on a path-decoded file when an aggregate computes a value, such as `group status { s = sum(bytes + 1) }` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `len()` of an array inside an aggregate on a path-decoded file, which failed with a type error ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix zone map pruning that dropped matching chunks for a comparison with a non-integral literal, such as `x < 1.5` on an int field ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix zone maps of a path-decoded file that claimed a field named `ts` or `dur` was in every record.
  A filter could skip the check on records without it ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a raw export with `select` of nested or dotted paths that wrote empty objects ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix schema detection that dropped a last line without a trailing newline.
  A one-line file read as generic ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix schema detection that read no line at all when the first record passed 16 MiB ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix index builds for files whose indexes live in different folders, such as the `split/` copies that `dftracer_view` makes beside unsplit files.
  Before, every file went into the index of the first file.
  The others then read as one whole-file chunk and a first query lost rows at random.
  Each folder's files now build into their own index ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a line whose newline opens the next gzip member that was lost when the read of its range ended inside the line ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix the last line of a file with no final newline, which was not read.
  Both fixes apply to the View scan and the index build ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix duql `distinct` with no keys, which returned rows with no columns.
  It now keeps every column ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a crash when a View whose files share an index path that does not open reads its schema tree ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix duql `select name | group name { n = count() }`, which failed with "no column '__duql_agg_0'" ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `dftracer_view --duql` so it writes the matched events, not a table, when the query reads `let`s or row sets only to filter the scan.
  An example is `fhash -> files.path like "/scratch/%"`.
  An arrow condition on the value of the row set alone runs as the key set it selects ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix the first-touch export pass, which did not honor `cancel_when` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix an array field that only a function reads, such as `len(xs)` in a `derive`, which read as null after the duql scan.
  It is now a List column ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix an object field that read as null after the duql scan.
  It is now its leaf columns ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a crash in duql `pivot` with an integer key column ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a LazyFrame join, or other multi-plan collect, over a schemaless View that could pair column names with the wrong columns.
  This happened when the records of a batch lacked a declared field.
  A missing field is now null ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a duql `agg { n = count() }` after a stage that leaves the scan, such as `sort`, which gave a null count ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a duql `select` that names one field twice, such as `select name, tag = name`, which gave an empty second column ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix after-scan duql stages over schemaless records that failed when a batch held only some of the fields.
  A missing field is now null in that batch ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix an indexed path such as `sizes[0]` that read null after the scan ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `View::call_tree` so rows come in `pid`, `tid`, start order.
  The `parent_id` numbers no longer depend on the number of workers ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix the comparison of an integer column with a fraction or with a value outside the range of the column (`dftu_series_compare`, `expr_cmp`).
  Before, `x == 2.5` held for 2 ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `LazyFrame::slice(offset, len)` with a `len` near `INT64_MAX`, which returned no rows ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `eval_many` over more than 65,536 rows without a parallel backend, which failed ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a filter on a bool field such as `ok == true` that skipped chunks with matching records ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix aggregations with a bool filter that read `true` as the number 1 ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix the fast equality path of the reader, which read `args.<name>` for a top-level field present as `null` ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix a query tree printed back to text, such as in index pruning and caches, so a string literal that holds `"` stays parseable ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix `any(path)` filters on JSON records and on canonical JSON text of arrays and objects that read memory past its lifetime ([60b4b7e](https://github.com/llnl-asr/dftracer-utils/commit/60b4b7eda83a)).
- Fix an incremental aggregation build that shrank the stored time bounds of the aggregation tier to those of the files it added ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix an index build after a query answered from the aggregation tier that failed with "Cannot upgrade RocksDB instance ... from read-only to read-write".
  This happened in one process ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make distributed builds honor `AggregationConfig.group_by_file` on the workers.
  Before, the file stayed in every key ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fail with an IO error when an indexed trace export cannot open or write its output.
  This covers `sink_trace` with `build_index` and `dftracer_view -o`.
  Before, `dftracer_view -o /dev/null` hung forever and the library could abort ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix the server decoded-member cache, which evicted large members on almost every request.
  The cache now uses one total budget.
  On a 1.4M-event trace, a timeline window request drops from 98 ms to 22 ms ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix filters on `dur`, `pid` or `tid` that could skip chunks with matches when records also had an `args` key of the same name.
  The `dur` filter now also covers `args.dur` of records without a top-level `dur`.
  Rebuild indexes to pick up this fix ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix a server crash when a request matched nothing and sorted its rows.
  Such a result now has no rows ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix the trace viewer error "No indexed events with a valid time range" on traces whose clockless events, such as CUDA activity, have `ts` 0.
  The timeline now starts at the first start above 0.
  Those events keep `ts` 0 in queries and time filters.
  Rebuild indexes made before this change ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix `resolved.exec`, `resolved.cmd` and `resolved.cwd` filters that matched nothing or the wrong field.
  They now read `exec_hash`, `cmd_hash` and `cwd` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix `TraceReader` filters that dropped matching lines written with a space after the colon, such as the default output of Python `json.dumps` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix the Arrow reader so that an equality on a top-level field of a generic record, such as `op == "read"`, returns the matching rows.
  The reader now matches a bare field at the top level before `args` ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix misaligned pooled coroutine frames on targets such as Apple arm64.
  A directory scan could hit undefined behavior ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix a crash on a group-by or aggregate over a column that the scan does not produce.
  It now raises an error that names the column ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix `phase("metadata")` queries that lost records.
  For example, `phase("metadata").query('name == "thread_name"')` could return nothing ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix `TraceReader` queries so that they return matching metadata lines.
  An equality on a dotted path with a long value, such as `args.name == "..."`, now matches ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Keep every `thread_name`, `process_name`, `PR` and `CM` record in a pruned export from `sink_json` or `dftracer_view --output`.
  Before, a trace viewer showed unnamed threads ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make `time_range(begin, end)` keep exactly the data events that start in `[begin, end)`.
  Before, a collect or aggregation also returned other events of the chunks it read.
  Busy, concurrency, utilization and active take every event that overlaps the window, clipped to it.
  Utilization divides by `end - begin`.
  Other aggregates of the same query take the events that start in the window.
  Metadata records are not windowed ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make occupancy aggregates with `time_bucket` clip each event to every bucket it overlaps.
  Utilization divides by the bucket width.
  Before, a long event added all of its duration to the bucket where it started.
  The aggregation tier now buckets an event by its start, as the scan does, and not by its midpoint ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make a `resolved.*` filter, such as `resolved.fpath == "/x"`, match every event whose hash resolves to the name.
  Before, traces that hashed one name differently could lose events, and two builds of the same traces could differ ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix re-indexing a trace, which left old data behind.
  A re-index now replaces all data of the file ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix a capability bit that was lost when it was added in the same write that registered a file ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Make a View fail with an error that names the index when its `resolved.*` rewrite cannot read the hash tables.
  Before, it matched no event and gave no error ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).
- Fix queries with `NOT` that could skip a chunk with matching records.
  Such chunks are now read ([73b14b1](https://github.com/llnl-asr/dftracer-utils/commit/73b14b19747a)).

## [0.0.13] - 2026-09-30

### Changed

- Turn off precompiled headers by default (`DFTRACER_UTILS_ENABLE_PCH`).
  Pass `-DDFTRACER_UTILS_ENABLE_PCH=ON` to keep them in a local build.
- Make the separate builder API internal, so the server, `dftracer_view`, `dftracer_run`, the statistics tools and the C ABI run on the same `View`.
- Make the trace `View` (C++) and `TraceViewer` (Python) a `LazyFrame` over a trace scan.
  Generic ops such as filter, select, sort, head and join chain on it.
  The scan absorbs filters, projections, group keys and a trailing row window at plan time.
- Make the trace terminals `call_tree`, `flamegraph`, `containment`, `flamegraph_partial`, `aggregate_partial`, `sink_json`, `sink_trace` and `materialize` return lazy results.
  Use `collect_all` to run several of them over one shared scan.
  Python adds lazy `[]`, `LazyScalar` reductions and `LazyResult`.
- Rename the C++ `View` terminals to follow the `LazyFrame` names.
  `export_json`, `export_trace` and `export_counters` become `sink_json`, `sink_trace` and `sink_counters`.
  `merge_partials_to_table` becomes `merge_partials`, `limit` and `offset` become `head` and `slice`, `occ_cell` becomes `resolution` and `schema()` becomes `column_info()`.
- Change C++ `View::collect()` to return the `DataFrame` and `View::lazy()` to return the plan.
  Use `collect_all` or `TraceSession` for several outputs over one scan, and `View::branch` to attach caller folds.
- Change `DaskTraceViewer.occ_cell` to `resolution`.
  Its `offset` and `limit` now trim the merged result instead of each shard.
- Make the index cover every flat args field by default.
  Numbers use a per-chunk min and max, and strings use a per-chunk bloom filter up to 256 distinct values.
  A filter on any arg now prunes chunks.
  On a 2M-event trace the index is 1.2 MB, against 8.7 MB for the previous named defaults.
  An existing index rebuilds once.
  Pass `--no-auto-dimensions` to `dftracer_index` to opt out.
- Make the dataframe engine faster.
  At 10M rows on an Apple M4 Pro it is ahead of pandas on every benchmark row, ahead of polars on all but two rows and ahead of DuckDB on all but one row.
  Group-by, join, sort, filters, comparisons, string predicates, casts, gathers, rolling windows, `//`, `%`, `**`, the dictionary encoder and quantile are faster.
- Make `Series.rolling(...).mean()` and the other windows write their output in place and run a chunk per thread.
- Make a plan over a resident frame run whole-column for every op.
  It no longer streams through a spool or spills to disk.
- Make a shard set aggregate its shards concurrently, as many at once as the spill budget gives each at least 64 MB.
  Make `collect_all` split each plan's spill budget across the plans it runs together.
  Concurrent work stays within the budget.
- Apply `--select` to raw event queries as an SQL-style column projection.
- Change the comparator to compare all events by default, not only POSIX and STDIO events.
- Change the on-disk index to a sharded immutable index with a registry keyed by full path.
  The on-disk index format changes.

### Added

- Publish a `<tag>.postN.dev0` prerelease to PyPI on every merge into `develop`.
- Add a columnar `DataFrame`, `Series` and `LazyFrame` engine that works as a drop-in for pandas and polars.
  Use `import dftracer.utils.pandas as pd` or `import dftracer.utils.polars as pl` and most code runs unchanged on SIMD kernels.
- Add these pandas features.
  `loc`, `iloc`, `at`, `iat` and `set_index` with a named index column and copy-on-write assignment.
  `groupby` objects with the `agg` forms, column selection such as `groupby(k)["v"]` and a Series key.
  Group-wise `cumsum`, `shift`, `rank`, `head`, `nth`, `ffill`, `bfill`, `rolling`, `expanding`, `ewm`, `take`, `sample` and `resample`.
  `apply` and `map` compiled into the engine, with a warning when Python must run per row.
  The `str` and `dt` accessors, `tz_localize`, `tz_convert`, `merge`, `nlargest`, `value_counts`, `mode`, `compare`, `pivot_table` and `describe`.
- Add the polars spellings `select(Expr)`, `with_columns`, `over` and the `str` namespace.
- Let `Series` masks combine with `&`, `|` and `~`.
  `==` and `!=` return a mask, and a Series is unhashable as in pandas and polars.
- Add a hash join on `DataFrame`, `LazyFrame`, the C ABI and Python with `join` and `merge`.
  It supports inner, left, right, outer, semi, anti and cross joins.
  A plan's join sends its build keys to the scan, which prunes the chunks that cannot match.
- Add `explain()`, `schema()` and `output_schema()` to `LazyFrame` plans.
  They do not run the plan.
- Add `memory_budget` and `auto_spill` to bound the memory of every pipeline breaker in a plan.
- Let a plan use a source of your own (`Source` or `dftu_source_vt`) registered by name.
- Let a plugin add its own plan step with `dftu_node_register` or `LazyFrame.op`.
- Add a `frame_op` plan step for `unnest`, `partition_id`, `compare_agg`, `window`, `gap_fill`, `asof`, `interval`, `concat`, `union`, `pivot` and `to_dummies`.
- Add the aggregates `prod`, `cumprod`, exact group `median` and `quantile`, `unique(subset)`, a per-column `reduce` and a whole-frame `group_by()`.
  `first` and `last` are exact across a parallel merge.
- Let group keys have any type, including Binary and Float16.
  Null keys form their own group with `dropna=False`.
- Let a plugin transform the batch that every later plugin receives with `transform`.
  A plugin reports and releases what it holds against the memory budget with `bytes` and `reclaim`.
  The same budget measures a plugin node or slice under a plan.
- Add `Indexer(bloom=BloomConfig(fields=...))` in Python to name extra args fields to index.
- Add a counter timeline track, a per-counter pid and tid breakdown, bounded-density serving and active-time statistics to the web trace viewer.
  The track handles malformed values.
- Let rectangle selection in the viewer scope the analysis to a time range, lanes and rows.
  The viewer shows aggregated (`ph=3`) events with uniform extrapolation and labels them "aggregated" in tooltips.
- Let an event-map file remap the DLIO event category and name.
- Export `DaskTraceViewer` from the Python `dask` module.
- Add `TraceViewer` and `View` Python bindings that expose the query DSL and portable C-API glue.
- Add subsumption-based simplification, string-match operators and `resolved.*` virtual fields to the query DSL.
- Add materialized views with rollups and tier-served aggregation.
  Add a new `AggregationFold` and more aggregation operators.
- Add the occupancy aggregates `busy`, `concurrency`, `utilization` and `active`, which measure wall-clock busy time and parallelism.
  They do not double-count overlapping durations.
  They take no field and always work over `dur`.
  They work per group and time bucket and merge across files and ranks.
  They are available in the `View` and `TraceViewer` `agg`, the `dftracer_view --agg` CLI and the typed `AggOp` enum.
- Add multi-member split, merge and reorganize for intra-file parallelism.
- Let every `<bytes>` CLI flag accept a unit suffix such as `64MB`, `1.5GiB`, `512KB` and `8kb`.
  Units are 1024-based and `b` means bits.
  A bare number keeps the legacy unit of the flag.
- Let every `<s>` CLI flag accept a duration suffix such as `30s`, `5m` and `1.5h`.
  A bare number keeps the legacy unit of the flag.
- Let the byte and duration arguments of the Python API accept a number or a string.
- Add `dftracer_server --timeout` to bound the server uptime and then shut down gracefully.
  The flag accepts a humanized duration.
  The default `0` disables the limit.
- Add full type coverage to the public Python surface, including `Series`, `DataFrame`, the viewers, the jit DSL and a generic `TaskHandle`.

### Removed

- Remove the previous plugin ABI.
  A plugin built against it does not load.
  Rebuild it against `dftracer/utils/plugins/abi/plugin.h`.
  The `abi_version` is now a hash of the header, and a plugin built against another version is refused at load.
- Remove the C++ `AggregatedView`, the public constructor of `ViewSession`, `View::join` and `trace/views/result_batch.h` (`collect_batch`).
  Use `View` plans, `View::branch` and `LazyFrame::join` instead.
- Remove the Python `AggregatedTraceViewer` and `SessionView`.
  Use `TraceViewer`, which is now a `LazyFrame`.
- Remove the `name=` argument from `Runtime.submit()`.
  The task name now comes from the callable and its call-site source location.
- Remove the legacy per-line visitor scan path.
  The batch-native fold-fusion indexer replaces it.

### Fixed

- Fix memory detection on macOS.
  It now reads the free and inactive pages, where the automatic budget assumed 1 GB.
- Fix the version of prerelease Linux wheels, which showed `.post1.devN`.
  The wheel version now sees tags.
- Fix sketch quantiles (`pct` in a plan and `DDSketch`) that return `-inf` when a bucket holds more than 65535 values.
- Fix column-column arithmetic that drops nulls.
- Fix the gather of a Bool column, which read by byte instead of by bit.
- Fix `group_by` returning groups out of first-seen order after a parallel run.
- Fix a plugin node that answers asynchronously when the data is resident.
- Fix `df.groupby(series)` raising a `SystemError`.
- Fix a scan of an unindexed multi-member trace that reads the lines at each member boundary twice.
- Fix `dftracer_index --dimensions` doing nothing because the build dropped the index extra args fields.
- Fix the index build dropping nested fields.
- Fix merged chunk stats that order numeric min and max as text.
- Fix a float literal such as `pid == 1.0` that pruned matching chunks.
- Fix session containment branches that ignore `phase()`, so `phase(Events)` now drops aggregated records.
- Fix session aggregations with string-arg predicates or transformed keys that write rollups which later reads serve wrongly.
- Fix a blocking `get()` on a finished coroutine task that can miss its result.
- Fix `LazyFrame::collect()` on a temporary plan reading the freed plan.
- Make the export sink of a plan flush when the export ends.
- Fix two data races in libdeflate kernel choice and in the member decode cache of the reader.
- Make an expression that the column type cannot take raise an error.
  Such an expression, such as a string column compared with a number, used to give a null column that crashed a later `group_by`.
- Fix the indexer to recover a chunk when one bad line collapses the batch parse.
- Fix the indexer to close cached index handles before it removes a stale index.
- Fix the server viz summary to zero-fill simdjson padding.

## [0.0.12] - 2026-07-13

### Changed

- Make wheels smaller.

### Added

- Add an interactive web trace viewer that draws traces on a canvas.
  The viewer has timeline node grouping (#93), app-span events and a timelapse axis for multi-run traces.
  A new server visualization API feeds the viewer.
- Show histogram bounds in `dftracer_stats`.
- Let the indexer detect and rebuild stale indexes when the source trace changes.
  Read consumers and the server use the rebuilt index.

### Fixed

- Fix scatter-gather writes so they complete past the `IOV_MAX` limit.
- Fix wrong file counts in the comparator.
- Fix a deadlock in multi-process `dfanalyzer`.
- Fix a server run break with stale indexes and app-span complement.
- Make `dfanalyzer` distributed indexing skip files that are already indexed.

## [0.0.11] - 2026-07-05

### Changed

- Change the core runtime, I/O and single-op utilities to use the `Result<T>` error model.

### Added

- Add the `dftracer_validate` CLI tool.
- Add the `Result<T>` error model with `DFT_TRY`.
- Add typed Python exceptions that map from `ErrorCode`.
- Add a DLIO preset to the comparator.
- Add an `atexit` shutdown to the Python bindings.

### Fixed

- Fix the GIL ordering in the Python bindings.
- Fix an executor shutdown deadlock from a lost wakeup.
- Fix a segfault from races in the object pool.

## [0.0.10] - 2026-06-08

### Added

- Add distributed HLM filtering and time bucketing.

## [0.0.9] - 2026-06-07

This release changes only the build and CI.

## [0.0.8] - 2026-06-05

### Added

- Add portable-wheel support with the `DFTRACER_UTILS_LOCAL_PACKAGES` option.

### Fixed

- Fix `to_chars_double` on macOS with a portable fallback.
- Improve zstd handling.

## [0.0.7] - 2026-05-22

This release changes only the build and CI.

## [0.0.6] - 2026-05-21

Large feature release with the async core, indexing, aggregation, query and server capabilities.

### Changed

- Change utilities to use the query DSL.
- Store indexes in RocksDB instead of SQLite.
- Replace xxhash with FNV-1a for hashing.
- Make parsing, scanning, serialization, compression, zero-copy I/O and the aggregation pipeline faster.

### Added

- Add a query DSL that filters trace events while it reads raw bytes.
  The reader skips whole chunks at file level.
- Add an async HTTP server that exposes trace query APIs.
  Responses stream to the client.
- Add Arrow data interchange with nanoarrow.
  Data crosses the Python boundary without a copy.
- Add a Python `Runtime` with async `submit()` and `TaskHandle`.
  The `Runtime` accepts Python callables and drives the Indexer.
- Add streaming iterators and a `StreamingUtility` with Arrow output to Python.
- Add a Dask plugin.
- Add `dftracer_aggregator` with profile and system-event aggregation, custom metrics, offset tracking and time-bucket persistence.
- Add `dftracer_comparator` to compare trace metrics.
  It injects trace metadata into root SUMMARY rows.
- Add a bloom-filter multi-index and manifest (`.midx`) sidecars.
- Add a DFT view system with bloom filtering and predicate support.
- Add an `index_threshold` option that skips bloom and manifest data for small files.
- Add parallel statistics with DDSketch and log2 histograms.
- Add `dftracer replay` for provenance-based, semantic trace reorganization.
- Add a call-tree utility with MPI support.
- Add DLIO config generation.
- Add a C++20 coroutine core with async I/O backends (io_uring and kqueue), pipelines, a task system, heterogeneous `when_all` and `when_any` and channels.

### Fixed

- Fix memory leaks in `channel` and `when_all`, `when_all` await-ready races and channel lifetime management.
- Fix gzip inflater trailer and window-reset handling.
- Fix double counting when an index already exists.
- Fix type-safety, lifetime, concurrency and portability issues.
- Fix endianness and query fallback handling.

## [0.0.5] - 2025-10-21

### Fixed

- Fix wheels so they link their libraries correctly.
- Fix promise fulfillment and exception handling in pipeline executors.
- Make reading on from a gzip stream faster.

## [0.0.4] - 2025-10-20

### Fixed

- Fix a race condition from mutation of shared state.
- Make the source distribution include `cmake` and `tests` and exclude unneeded files.
- Fix CLI options in the docs.

## [0.0.3] - 2025-10-20

### Fixed

- Fix the versioning scheme and the link to the DFTracer GitHub repo.
- Exclude unneeded files from the source distribution.

## [0.0.2] - 2025-10-19

### Fixed

- Add `setup.py` to fix a Python versioning issue.
  Ship the scripts that Python publishing needs.

## [0.0.1] - 2025-10-19

Initial release.

### Added

- Add core trace tools `dftracer_index` (gzip and tar indexers), `dftracer_merge`, `dftracer_split` (with `--verify`), `dftracer_event_count`, `dftracer_info` and `pgzip`.
- Add a coroutine-driven task scheduler, executor and pipeline framework.
  The framework has progress callbacks and multiple-output support.
- Add gzip block-boundary-aware reading with prefix-based hashes and checkpointing.
- Add Python packaging and Sphinx documentation with a C++ API reference.

[Unreleased]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.14...HEAD
[0.0.14]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.13...v0.0.14
[0.0.13]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.12...v0.0.13
[0.0.12]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.11...v0.0.12
[0.0.11]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.10...v0.0.11
[0.0.10]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.9...v0.0.10
[0.0.9]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.8...v0.0.9
[0.0.8]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.7...v0.0.8
[0.0.7]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.6...v0.0.7
[0.0.6]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.5...v0.0.6
[0.0.5]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.4...v0.0.5
[0.0.4]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.3...v0.0.4
[0.0.3]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.2...v0.0.3
[0.0.2]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.1...v0.0.2
[0.0.1]: https://github.com/llnl-asr/dftracer-utils/releases/tag/v0.0.1
