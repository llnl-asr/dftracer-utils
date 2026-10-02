:description: Build the .dftindex sidecar once and keep it fresh so later queries skip decompression, from Python, C++, or the CLI.

Build and use a trace index
============================

Reading a trace cold means decompressing it end to end. An index is a sidecar
built once per trace directory that lets a later query skip most of that work:
see :doc:`../../concepts/indexing-and-pushdown` for what it holds (gzip
checkpoints, per-chunk zone-map statistics, and bloom filters) and how a query
prunes chunks with it. This page is about building one and keeping it fresh,
not the pruning mechanics.

The index lives in a ``.dftindex`` directory next to the trace files it
covers (or wherever you point ``index_dir``); it is a RocksDB store, not a
single file. Building it is idempotent: run the same build again on unchanged
traces and it does nothing, so you can call it at the top of every script
without worrying about redundant work.

.. tab-set::

   .. tab-item:: Python

      ``Indexer`` builds the checkpoint and bloom-filter tiers (and,
      optionally, a pre-aggregated tier). ``ensure_indexed()`` checks what is
      already built and only builds what is missing:

      .. code-block:: python

         import dftracer.utils as dft

         with dft.Indexer(files=["trace.pfw.gz"]) as ix:
             status = ix.ensure_indexed()
             print(status.total_files, status.ready, status.needs_work)

         # Or point at a whole directory:
         with dft.Indexer("traces/") as ix:
             ix.ensure_indexed()

      ``ensure_indexed()`` is ``resolve()`` (check what needs work) followed
      by ``build()`` (build only that) in one call; call ``resolve()`` on its
      own to inspect status without triggering a build. ``force_rebuild=True``
      on the constructor rebuilds every tier regardless of what already
      exists.

      Both ``require_checkpoint`` and ``require_bloom`` default to ``True``.
      Add the pre-aggregated tier with ``require_aggregation``:

      .. code-block:: python

         from dftracer.utils import Indexer, AggregationConfig

         with Indexer(
             "traces/",
             require_aggregation=AggregationConfig(time_interval_ms=1000),
         ) as ix:
             ix.ensure_indexed()  # one fused pass builds all three tiers

      The bloom tier always covers ``name``, ``cat``, ``pid``, ``tid`` and the
      file, host and command hashes. It also indexes other paths of each
      file, most frequent first, while their estimated evidence bytes fit
      ``stats_share`` (default 0.05, so 5%) of the file's compressed size, at
      least 8 MiB. ``path_budget`` is an optional count ceiling on top of
      that; 0 (the default) means no count limit. A number gets a
      per-chunk min/max, which lets range and equality filters
      (``size > 4096``, ``step == 500``) skip chunks, and a string gets a
      per-chunk bloom filter while the chunk holds at most
      ``auto_max_distinct`` (default 256) of its values. A string with more
      values than that keeps no bloom in that chunk, so a filter on it still
      reads the chunk unless its min/max rules the chunk out. A path outside
      the cap can still be filtered on; it just never skips a chunk.

      Evidence that cannot skip a chunk is not kept: a path whose min and max
      are the same in every chunk and that has no bloom. A chunk where a kept
      path has no value is recorded as absent, with no bloom. A test that
      needs a value of the path (``==``, ``in``, a range, ``exists``) skips
      that chunk; ``!=``, ``not in``, ``not exists`` and ``is null`` never
      skip on absence. Each chunk bloom is sized for the chunk's distinct
      values. Evidence of an args key prunes ``args.<k>`` queries. A bare name
      that a record may hold at the top level reads no args evidence, so
      ``id == 5`` on records with both a top-level ``id`` and an ``args.id``
      is never pruned by ``args.id``. The cap bounds the evidence bytes of the
      index, not the build's memory, which the memory budget still bounds.

      ``BloomConfig`` changes this. ``fields`` names args fields to index
      in full (a bloom filter and min/max per chunk, no cap), including
      nested ones (``"io.off"``), named as a filter names them (``"size"`` or
      ``"args.size"``). ``stats_share`` and ``path_budget`` set the
      cap described above; both can also be set per record schema with
      ``index: {stats_share: 0.2, path_budget: 50}``, and a schema setting
      overrides the build's. A ``stats_share`` outside (0, 1] or a negative
      ``path_budget`` is an error. An index built without a requested field,
      or with other settings, rebuilds its zone map, bloom and count evidence
      once (postings, stats and the catalog are kept) on the next
      ``ensure_indexed()``. The new evidence format also rebuilds existing
      evidence once.
      Example:

      .. code-block:: python

         from dftracer.utils import BloomConfig, Indexer

         with Indexer("traces/", require_bloom=BloomConfig(fields=["io.off"])) as ix:
             ix.ensure_indexed()

      Each field costs index space and build time in proportion to its
      distinct values per chunk, so index the fields you filter on, not every
      arg.

   .. tab-item:: C++

      A plain ``View`` query builds the index itself the first time it touches
      a genuinely unindexed file - see :ref:`indexing-first-touch` below. For
      an explicit build (warming the index ahead of serving traffic, or
      refreshing one that already exists), use ``Indexer``
      (``dftracer/utils/index/indexer.h``), the same path the Python
      ``Indexer`` drives:

      .. code-block:: cpp

         #include <dftracer/utils/index/indexer.h>

         using namespace dftracer::utils::index;

         IndexerOptions options;
         // Args fields indexed by name, besides the automatic ones.
         options.bloom->fields = {"io.off"};

         Indexer indexer = Indexer::open({"trace.pfw.gz"}, options);
         IndexStatus status = indexer.build();  // blocks on the default runtime
         // status.indexed is 0 when every file was already fresh.

      ``build`` skips fresh files, so it is idempotent like the Python and CLI
      paths; ``rebuild`` rebuilds every file. Inside a coroutine, call the
      async form instead: ``co_await indexer.build(scope)``.

   .. tab-item:: CLI

      ``dftracer_index`` builds the index for a directory (or the files it
      discovers under it) without any Python or C++ in your own code:

      .. code-block:: console

         $ dftracer_index --directory traces/
         $ dftracer_index --directory traces/ --force   # rebuild every tier

      Selected flags (see ``dftracer_index --help`` for the full list):

      .. list-table::
         :header-rows: 1
         :widths: 32 68

         * - Flag
           - Meaning
         * - ``-d`` / ``--directory``
           - Directory to scan for ``.pfw``/``.pfw.gz`` files (default ``.``)
         * - ``--index-dir``
           - Directory for the ``.dftindex`` store (default: next to the data)
         * - ``-f`` / ``--force``
           - Force index recreation even if already built
         * - ``--checkpoint-size``
           - Checkpoint size for gzip indexing, in bytes
         * - ``--dimensions``
           - Args fields to index by name (bloom filter and min/max, no
             cap), nested ones included, comma-separated, e.g.
             ``io.off,mode``
         * - ``--schema``
           - A registered record schema id for every file (default:
             detected per file)
         * - ``--memory-budget``
           - Bytes the build may hold at once (default: about a third of
             available memory); accepts units such as ``4GB``
         * - ``--stats-share``
           - Fraction of a file's compressed size that automatic evidence
             may use, in (0, 1] (default 0.05, at least 8 MiB)
         * - ``--path-budget``
           - Optional ceiling on how many other paths get evidence (default
             0, no count limit)
         * - ``--expected-entries``
           - Expected entries per chunk, for bloom filter sizing (default 1024)
         * - ``--false-positive-rate``
           - Bloom filter false positive rate (default 0.01)
         * - ``--executor-threads``
           - Worker threads for parallel indexing (default: CPU core count)

Reuse and incremental builds
-----------------------------

Nothing you write needs to track whether a trace has already been indexed.
Every build entry point - ``Indexer.ensure_indexed()``,
``Indexer::build``, and ``dftracer_index`` without ``--force`` -
checks the existing ``.dftindex`` store first and only does work for files
that are missing or out of date. Add new trace files to a directory and
re-run the same build command: only the new files get indexed, and the
existing ones are left alone.

.. _indexing-first-touch:

A first query on a fresh file builds the index for you
-----------------------------------------------------------

You do not have to index before you query a file that has never been indexed.
The first aggregation query against a genuinely fresh file - no ``.dftindex``
for it yet, and a plan without a time range - takes a one-pass "bootstrap"
that both answers the query and builds the full index (members, bloom
filters, stored row sets) as a byproduct, so the query never pays for a separate
eager build. This holds for any record schema, a path schema included; the
index records the schema the file was decoded with:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         # No prior Indexer call needed: the first touch builds the index.
         df = TraceViewer("traces/").group_by("cat").agg("count").collect()

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         // .get() blocks a non-coroutine caller like main(); co_await
         // instead inside async code.
         View view = View::from_directory("traces/").get();
         auto df = view.group_by({GroupKey::cat()})
                       .agg({AggSpec(AggOp::Count)})
                       .collect()   // -> coro::CoroTask<DataFrame>
                       .get();

This bootstrap only fires for that clean-first-touch case. Once an index
exists, every View checks it when the View is created: a file whose size or
modification time differs from the index record, or whose checkpoint size
changed, is indexed again through the same resolver ``dftracer_index`` uses,
which replaces all of that file's evidence, before the View reads the index.
A file already checked in this process with the same size and time costs one
``stat``. So a View over a trace that was appended to or replaced reads the
new content; the first View after the change pays the rebuild. A change that
keeps both the size and the modification time is not seen.

Upgrading the library can also invalidate an index: each on-disk index records
the ``FORMAT_VERSION`` it was built with, and a build with another version
rebuilds the index rather than reading it. Changing a bloom setting (such as
the false positive rate) rebuilds only the extensions it affects. Changing the
aggregation config (such as ``time_interval_ms``), or a change to a trace whose
events are in the aggregation tier, clears that tier and aggregates every file
again, and leaves the other extensions in place: the tier merges all files'
events without recording which file each came from, so one file's share cannot
be replaced. Until the next build, queries the tier cannot answer exactly read
the traces instead. An index directory is cheap to rebuild, so this needs no
action from you beyond letting the next build run.

The tier builds for files of any record schema. For a path schema it answers
the group keys ``name``, ``pid`` and ``tid`` (its name, entity and lane roles)
and aggregates of its duration and time fields, when each role
field is required, the name is a ``string``, the others are ``int``, times are
in ``us`` and the source has no ``data`` row set. Any other query reads the
traces.

Inspect, rebuild or drop one extension
--------------------------------------

The pruning data is split into extensions: ``zonemap`` (value ranges),
``bloom``, ``counts`` (value counts), ``postings`` (name to chunks) and
``dft.stats`` (statistics) and ``dft.metadata`` (metadata records per
chunk), next to ``core.members`` (gzip members), ``core.catalog`` (the path
catalog) and ``core.rowset`` (the stored row sets of the record schema's
source). ``extensions`` chooses which of the
first four a build makes; the default is all four. Use the manifest to see what each file
has, ``explain`` to see why a filter reads the chunks it reads, and
``rebuild_extension`` or ``drop_extension`` to act on one extension without
rebuilding the rest.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         import dftracer.utils as dft

         with dft.Indexer("traces/") as ix:
             ix.ensure_indexed()
             for f in ix.manifest():
                 print(f["path"], [e["name"] for e in f["extensions"] if e["current"]])

             for f in ix.explain('name == "read"'):
                 print(f["path"], "reads", f["read"], "of", f["chunks"])
                 for e in f["extensions"]:
                     print("  ", e["name"], "rules out", e["removed"])

             ix.drop_extension("counts")      # removed from every file
             ix.ensure_indexed()              # built again: still requested
             ix.rebuild_extension("bloom")    # rewrites bloom only

         # Build without counts; a later build does not add it back.
         with dft.Indexer("traces/", extensions=["zonemap", "bloom", "postings"]) as ix:
             ix.ensure_indexed()

   .. tab-item:: C++

      .. code-block:: cpp

         IndexerOptions options;
         options.extensions = {"zonemap", "bloom", "postings"};
         Indexer indexer = Indexer::open({"traces/"}, options);
         indexer.build();

         for (const FileExplain& f : indexer.explain(R"(name == "read")"))
             for (const ExtensionPrune& e : f.extensions)
                 std::printf("%s: %s rules out %zu chunks\n", f.path.c_str(),
                             e.name.c_str(), e.removed.size());

         indexer.rebuild_extension("bloom");
         std::string json = to_json(indexer.manifest());

   .. tab-item:: CLI

      ``dftracer_info --query detailed`` prints each file's extensions, and
      marks one whose build failed or that an older version wrote.

``explain`` reports, per file, the chunks the query reads for its data events
(the chunks a View row or aggregation scan reads) and, for each pruning extension, the chunks it would rule
out if it were the only one. It writes nothing. A dropped extension is built
again by the next build only while ``extensions`` names it. ``core.members``,
``core.rowset`` and ``dftracer.agg`` (the aggregation tier, rebuilt by a build
with ``require_aggregation``) cannot be rebuilt or dropped alone.

Index traces that are not dftracer traces
------------------------------------------

Any gzip of JSON lines can be indexed. A file whose first lines are not
dftracer events is indexed with the ``generic`` record schema: every field,
nested ones included, becomes a path, and filters on those paths skip
chunks. Detection is per file; set the schema to skip it or to override it:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         with dft.Indexer(files=["requests.ndjson.gz"]) as ix:
             ix.ensure_indexed()                     # detected as generic
             print(ix.explain('op == "read"'))       # chunks the filter skips

         with dft.Indexer("traces/", schema="dftracer") as ix:
             ix.ensure_indexed()

   .. tab-item:: C++

      .. code-block:: cpp

         IndexerOptions options;
         options.schema = "generic";  // empty detects each file's
         Indexer indexer = Indexer::open({"requests.ndjson.gz"}, options);
         indexer.build();
         std::string schema = indexer.files().front().schema;

   .. tab-item:: CLI

      .. code-block:: console

         $ dftracer_index --directory logs/ --schema generic

Read a generic trace with a ``TraceViewer`` (C++ ``View``) as usual.
Columns are named by path, without an ``args.`` prefix (``op``, ``io.off``,
``hosts.1``), and filters, ``select``, sorting, ``group_by`` on any path and
every aggregate read them:

.. code-block:: python

   tv = dft.TraceViewer("requests.ndjson.gz")
   tv.duql('op == "read" and io.off > 4096').select("op", "lat").collect()
   tv.group_by("op").agg("count", "sum:lat").collect()

The ``generic`` schema binds no time or duration field, so ``time_range``,
``time_bucket``, the occupancy aggregates (``busy`` and the others) and
``call_tree``/``flamegraph`` raise an error on generic records, and so do
the dftracer-only group keys ``io_cat`` and ``acc_pat``. The keys ``fhash``
and ``hhash`` group on the fields of those names; ``file_path``,
``file_name``, ``host_name`` and ``rank`` relabel them (``rank`` the entity
role) through the source's ``files`` (``fhash``, ``path``), ``hosts``
(``hhash``, ``name``) and ``ranks`` (``pid``, ``rank``) row sets, and raise
an error naming the row set when the schema's source defines none. Declare
your format as a record schema to use them. A viewer reads
files of one schema: mixing dftracer traces and generic files in one viewer
raises an error naming both; an empty file, or one of metadata only, takes
no vote. A directory viewer finds ``.jsonl`` and ``.ndjson``
files next to ``.pfw`` traces, plain or gzip, and so does the indexer.

.. _describe-your-own-record-format:

Describe your own record format
-------------------------------

A record schema declares the typed fields of a format: the fields a record
must hold (detection), the fields that play the time, duration and entity
roles (trace operations), and the fields indexed past the path budget.
Declare it as a YAML or JSON file, a Python class or a C++ class; all three
give the same schema (see :doc:`../../reference/record-schema`):

.. tab-set::

   .. tab-item:: YAML

      .. code-block:: yaml

         # schemas/access_log.yaml: nginx access logs written as JSON lines.
         id: access_log
         fields:
           remote_addr: {type: string}
           status: {type: int}
           ts_ms: {type: int, role: time, unit: ms}
           request_time: {type: float, role: duration, unit: s}
           upstream: {type: string, always_index: true}
           tags: {type: json, optional: true}

      Put the file where the index finds it: in ``<index_dir>/schemas/``,
      next to the ``.dftindex`` store (beside the data by default), or in a
      directory named by ``DFTRACER_SCHEMA_PATH`` (``:``-separated).

   .. tab-item:: Python

      .. code-block:: python

         from typing import Optional

         from dftracer.utils.schemas import Json, RecordSchema, field

         class AccessLog(RecordSchema, id="access_log"):
             remote_addr: str
             status: int
             ts_ms: int = field(role="time", unit="ms")
             request_time: float = field(role="duration", unit="s")
             upstream: str = field(always_index=True)
             tags: Optional[Json]

      The class registers the schema when Python defines it.

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/index/schema_class.h>

         namespace ix = dftracer::utils::index;

         struct AccessLog {
             static constexpr std::string_view id = "access_log";
             ix::Field<std::string, "remote_addr"> remote_addr;
             ix::Field<std::int64_t, "status"> status;
             ix::Field<std::int64_t, "ts_ms", ix::TimeRole<ix::TimeUnit::MS>>
                 ts_ms;
             ix::Field<double, "request_time",
                       ix::DurationRole<ix::TimeUnit::S>> request_time;
             ix::Field<std::string, "upstream", ix::AlwaysIndex> upstream;
             ix::Field<std::optional<ix::Json>, "tags"> tags;
         };

         ix::register_schema<AccessLog>("myapp");

Register the schema before the files are indexed, then index and read them
as usual:

.. code-block:: python

   with dft.Indexer(files=["access.ndjson.gz"]) as ix:
       ix.ensure_indexed()
       print(ix.explain('upstream == "u2"'))   # reads the u2 chunks only

   tv = dft.TraceViewer("access.ndjson.gz")
   tv.duql("""tags == '["api", "v1"]'""").group_by("status").agg("count").collect()
   tv.time_range(1_700_000_000_000_000, 1_700_000_001_000_000).collect()
   tv.time_bucket(1_000_000).agg("count").collect()

``time_range`` takes microseconds, as for dftracer traces; the schema's
``unit`` converts the field, and the time field's index skips chunks
outside the range. Records without a numeric time are left out of the
time operations but stay in plain row queries.

A field reads as its declared type: an ``int`` field keeps integers and whole
numbers and reads anything else as null, and a ``json`` field reads an
object or array as canonical JSON text (no spaces, keys sorted), so
``'["api", "v1"]'`` matches however the records space it. Index evidence
covers the leaves under a ``json`` field (``tags.0``), not the whole value;
``any(tags) == "v1"`` filters by element and skips chunks through that
evidence (see :doc:`duql`).

``schemas.explain(path)`` shows each schema's detection share for a file,
and ``TraceViewer.schema_tree()`` shows the paths the index holds with their
types, counts and declared fields.

Each file records the id of its schema, so every process that reads the
index must register the same schema; a missing one is an error that names
the id. Editing a schema rebuilds the files detected as it on the next
build. To read a file as another schema without rebuilding, name it on the
viewer: ``TraceViewer(paths, record_schema="generic")``, C++
``View::record_schema("generic")``, or ``dftracer_view --schema generic``.

Read records as schema class objects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A schema class also reads records back as objects, one per selected record,
after the viewer's filters and time window:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         for log in dft.TraceViewer("access.ndjson.gz").duql("status >= 500").rows(AccessLog):
             print(log.upstream, log.request_time)

      ``rows`` collects the fields first, so use ``collect()`` for bulk
      work.

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/typed_rows.h>

         namespace tv = dftracer::utils::trace::views;

         std::vector<AccessLog> logs =
             co_await tv::rows<AccessLog>(
                 tv::View::from_file("access.ndjson.gz").duql("status >= 500"));
         for (const AccessLog& log : logs)
             std::printf("%s %f\n", log.upstream->c_str(), *log.request_time);

      The scan workers decode each record straight into the class, which
      is about twice as fast as ``collect()`` followed by reading the same
      columns. A member whose value is missing or does not convert keeps
      its default (empty for ``std::optional``). Rows from different workers
      arrive in no fixed order. ``rows<T>(view, tv::LAZY)`` defers the read
      for ``collect_all()`` or a session. The class does not need to be
      registered to read rows.

Bound the memory an index build uses
-------------------------------------

An index build holds each file's chunk evidence until the file is written,
and several files are built at once. ``memory_budget`` caps that the same
way ``TraceViewer.memory_budget`` caps a scan: 0 (the default) is about a
third of available memory, and a byte count or a size such as ``"4GB"`` sets
it. Under the budget, fewer large files are built at a time, and a file
whose evidence outgrows its share writes its finished chunks to sorted runs
beside the index (``<index>.spill``) and commits them with the rest of the
file in one atomic ingest. The index holds the same data either way; a build
that spills is slower.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         with dft.Indexer("traces/", memory_budget="2GB") as ix:
             ix.ensure_indexed()

   .. tab-item:: C++

      .. code-block:: cpp

         IndexerOptions options;
         options.memory_budget = 2ULL << 30;
         Indexer::open({"traces/"}, options).build();

   .. tab-item:: CLI

      .. code-block:: console

         $ dftracer_index --directory traces/ --memory-budget 2GB

Memory not covered: the aggregation tier, and the per-file totals that grow
with distinct values rather than with the trace's length (file blooms, file
statistics, the path catalog).

Traces cut short
----------------

A job killed while writing its trace leaves the last gzip member cut short.
The index and every reader recover it: they decode that member up to the cut
and keep its complete lines, so a query sees every event up to the last full
line before the job stopped. Only a member that simply ran out of data is
recovered; a member whose data is corrupt is dropped as before.

The recovered lines cannot be checked against the gzip checksum, which was
never written. Such a trace is reported as truncated so you know its data
ends early: ``IndexStatus.truncated`` in Python and ``IndexStatus::truncated``
in C++ list it, ``Indexer::files`` flags it, and the build logs a warning with
the number of bytes recovered. A trace that is still being written looks
truncated too; once it grows, the next build indexes it again.

See also
--------

- :doc:`../../concepts/indexing-and-pushdown` for what the index holds and how
  pushdown uses it.
- :doc:`duql` for the predicates that get pushed down.
- :doc:`../../cpp_api/indexer` for the generated C++ indexer API reference.
- :doc:`../../trace-viewer` and :doc:`../data/dataframe` for reading the
  trace once it is indexed.
