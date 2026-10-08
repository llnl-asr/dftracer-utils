# Changelog

All notable changes to dftracer-utils are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with rules from [Common Changelog](https://common-changelog.org/), and this project adheres to [Semantic Versioning](https://semver.org/).

<!-- changelog-body -->

## [Unreleased]

### Changed

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
- Add `DFTRACER_UTILS_SPILL_DIR`, the directory of every spill file (collect, sort, group-by, unique and join); unset, the system temp directory is used as before. Point it at local disk on nodes where `/tmp` is held in memory ([115efb1](https://github.com/llnl-asr/dftracer-utils/commit/115efb18b5c3)).
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

[Unreleased]: https://github.com/llnl-asr/dftracer-utils/compare/v0.0.13...HEAD
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
