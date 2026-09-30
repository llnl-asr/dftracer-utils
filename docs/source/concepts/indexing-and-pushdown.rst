:description: What the .dftindex store holds - checkpoints, zone-map statistics, bloom filters - and how a query prunes chunks before decompressing them.

Indexing and predicate pushdown
================================

What this explains: what the ``.dftindex`` store holds, and why a query can
skip most of a trace file without decompressing it.

The index is built once, queried many times
--------------------------------------------------

Indexing (``index/``) reads a ``.pfw.gz`` trace once and writes a
per-directory RocksDB store (a ``.dftindex``) that later queries consult
instead of re-reading the trace. It holds the **checkpoints** that mark
where each compressed member of the gzip file starts, so a chunk can be
decompressed on its own, and four kinds of pruning evidence, each kept per
chunk and per field (a JSON path):

- ``zonemap``: the field's minimum, maximum and value type, and how many
  records carried it. It answers "could this chunk hold ``dur > 1000``"
  without reading an event. The ``ts`` zone also keeps a histogram of where
  the timestamps fall, and ``te`` (event end) gives each chunk's time span.
- ``bloom``: a bloom filter of the field's values, per chunk and for the
  whole file. A negative test is certain (no false negatives), so a chunk or
  file can be dropped on it; a positive one means "maybe".
- ``counts``: the field's values with their counts, up to a size cap. Over the
  cap the chunk keeps no counts and is never pruned on them.
- ``postings``: for ``name``, exactly which chunks hold each value.

A chunk the evidence does not cover, or a field nobody indexed, is always
read. The dftracer statistics that no pruning uses (duration sketches, per
name and category aggregates, file summaries) are kept separately for the
statistics tools.

Record schemas
--------------

A record schema names a record format through typed fields: the JSON paths
every record of it holds, the paths that play trace roles such as time and
duration, and the type each field reads as. Two are built in. ``dftracer``
requires ``ph`` and ``name`` and keeps the tuned dftracer event decoder.
``generic`` declares nothing: its records are decoded to every scalar leaf
by exact path, so any NDJSON can be indexed. Other formats extend one of
them in a spec file, a Python class or a C++ class, so a new format needs no
decoder code; its records decode as its parent's do, with each declared
field converted to its type and the role fields giving the time, duration
and lane that trace operations read.

A declared type is a reading rule, not a check: a value that does not
convert reads as null, and the index keeps the file's observed types, which
the schema tree shows next to the declared ones. A ``json`` field keeps a
whole object or array as canonical text for reading and exact comparison;
the index keeps evidence for the leaves below it, which is what a membership
filter needs, rather than for the whole value, which a filter rarely
compares.

Each file's schema is detected before it is indexed, from up to 1000
records at its start; metadata lines before them are read past, up to
16 MiB of text. A schema judges the records its ``data`` row set keeps, so
dftracer metadata and genesis ``RUN`` lines neither match nor miss it. A
record matches when it holds every required field, or, for a schema whose
fields are all optional, any declared field; paths resolve as queries read
them (a flat dotted key, an array index). Of the schemas at least 90
percent of their records match, the one with the most required fields
wins, then a user schema over a built-in, then the higher share, and
``generic`` when none does. dftracer requires ``ph``, ``name`` and ``ts``,
so Chrome trace events read as dftracer while a log that only has ``ph``
and ``name`` is generic. A file of metadata only is dftracer. The choice
is recorded with the file (``core.profile``), so a directory can mix
formats; without an index, detection is cached per file by path, size and
modification time. A View of many files takes the schema of the files with
records: an empty file, or one of metadata only, takes no vote. Asking for
another schema rebuilds the file.

A generic file's index holds the path catalog and automatic zone maps,
blooms and value counts for its most frequent paths, top-level or nested
(``op``, ``io.off``, ``hosts.1``), up to the path budget. A filter on those
paths skips chunks the same way a filter on a dftracer args field does.

The path catalog
----------------

For every file the index records each JSON path its data records hold, as
written (``args.size``, ``args.io.off``, ``args.hosts.1``, a top-level
``type``), with its type and the number of records in which it is not null.
Types follow a small lattice: integers that fit in int64, larger unsigned
integers, doubles, booleans and strings; numbers widen to the wider number,
and any other mix is ``mixed``. The schema of a View comes from the catalog,
so it needs no scan and sees a field even when only a few records carry it.

The catalog also bounds automatic evidence. Besides the fixed fields and the
fields you name, each file's most frequent args paths get a zone map and a
bloom, up to the path budget (1024 by default). A path outside the budget can
still be filtered on; it only cannot skip a chunk.

How the store is laid out
-------------------------

The store has a few families chosen by how data is read (``registry``,
``members``, ``granule``, ``blob``, ``postings``), not one per kind of data.
Every key starts with the extension that owns it, the kind of record and the
file, so one file's data for one extension is contiguous and can be replaced
with one range delete. Every value carries a small header naming its type and
version; a value the code does not recognise is ignored rather than misread.

A **manifest** records, per file and extension, the code version and the
configuration the data was built with. It is written in the same atomic write
as the data and is the only commit point of a build: data without a current
manifest entry is never used for pruning, and a changed configuration (such
as a bloom false positive rate) makes that extension rebuild on the next
``build``. Each extension has its own configuration hash and is checked on
its own, so an index may hold only some of the pruning extensions: one that
is absent or not current is not consulted, and the others still prune. The
whole store carries a format version; an index written by another version is
rebuilt, never read.

Predicate pushdown: prune before you decompress
------------------------------------------------------

A query does not scan a trace file end to end and then filter. Before any
chunk is decompressed, ``prune_file`` (``index/plan/prune.h``) decides which
chunks of one file *may* match, and every other chunk is skipped outright.
The View planner, the trace reader and a running scan's dynamic narrowing
all go through it.

Each kind of evidence is a **Condition** (``index/plan/condition.h``): the
file bloom, postings, counts, zonemap and chunk bloom. A Condition reads a
field's evidence only when the query has a leaf on that field, and answers
three questions about one leaf:
can the file match at all, which chunks may hold a matching event, and
which chunks hold only matching events. Every gzip member of the file starts
as a candidate, whether or not the index holds statistics for it. One
evaluator walks the query tree and combines the answers:

- at a leaf, the chunk sets of every Condition with evidence are
  intersected;
- ``AND`` intersects, ``OR`` unions;
- ``NOT p`` keeps every chunk except those where *every* event is proven to
  match ``p``;
- a pattern match (``like``, a regex) keeps every chunk.

When the View has a time range, ``prune_file`` then drops chunks whose
recorded time bounds lie wholly outside it.

The query and time evidence describe data events only, so metadata
(``ph="M"``) records take their own path through ``dft.metadata``, which
counts each chunk's metadata records and its context records (thread and
process names, ``PR``, ``CM``). A default View reads its source's ``data``,
which for dftracer traces leaves the metadata records out. A scan of every
record (``View::all()``) also reads every chunk that holds a metadata record.
A
``phase("metadata")`` query, and a ``TraceReader`` query, which matches
metadata lines as well as events, judge metadata by the record names and
fields ``dft.metadata`` keeps per chunk: ``name == "rename"`` reads no chunk
for its FH records, since no metadata record has that name, while
``name == "thread_name"`` reads the chunks that name threads.

Inside the chunks that survive, a raw pre-filter skips lines before they
are parsed. The query's ``==`` and ``in`` leaves give byte needles, the
value itself (``"read"`` with its quotes, or an integer's digits), so a
line without the needle cannot match whatever its spacing or key order;
``and`` requires every leaf's needle and ``or`` any of them. Only values a
writer spells one way give needles: strings of printable ASCII without
``"``, ``\`` or ``/``, and integers of three or more digits written plainly.
A scan that returns metadata records keeps every line, since those bypass
the filter, and a scan unit whose first 1024 lines mostly pass stops
checking. On the laghos trace this halves a selective name filter.

The rule behind all of this: **pruning never drops a chunk that holds a
matching event.** Every Condition answers with a superset of the true
matches, so an intersection is still a superset:

- **Bloom filters only prune, never confirm.** A negative bloom test is
  certain (no false negatives), so a chunk can be safely dropped on it; a
  positive test means "maybe," so the chunk is still decompressed and the
  predicate is evaluated for real against the parsed events.
- **An all-match proof must cover every record.** ``NOT`` can only drop a
  chunk when the statistics saw every line of it: the chunk's line count
  must equal its event count and every event must carry the field. A chunk
  with metadata records or events without the field is kept.
- **Missing evidence keeps the chunk.** A file without an index, a chunk
  without statistics or with corrupt time bounds is read in full.

This is the same principle a column-store's zone-maps and a Bloom-filtered
LSM tree both rely on: cheap, sound negatives eliminate the expensive path
(decompression and JSON parsing) for the overwhelming majority of chunks a
query does not care about, and the expensive path only ever runs on chunks
that survived every prune.

.. mermaid::

   graph LR
       Query["Query tree"] --> Eval["Evaluator<br/>(AND, OR, NOT)"]
       Conds["Conditions<br/>(postings, bloom, stats, histogram)"] --> Eval
       Eval --> Time["Time bounds<br/>(View time range)"]
       Time --> Candidates["Candidate chunks"]
       Candidates --> Scan["Fused scan<br/>(actual decompress + evaluate)"]

Row sets: querying by name without an index scan
------------------------------------------------

Trace events reference files, hosts, and executables by an interned hash
(``fhash``, ``hhash``, ``cwd``, ``exec_hash``, ``cmd_hash``), not by the
human-readable string. Writing a predicate directly against the hash is
unusable for a person; writing it against a live string comparison at scan
time would mean decoding every candidate row before it can even be tested.
The dftracer record schema instead has a duql source that declares row sets
over its metadata records (``files``, ``hosts``, ``strings``, ``ranks``; the
``genesis`` schema adds ``runs``, built from its ``RUN`` lines). A row set
that is a ``where`` plus a plain ``select`` is evaluated while the index is
built and stored per file as a native frame in ``core.rowset``, so reading
it later decodes no trace.

An arrow such as ``fhash -> files.path like "%/scratch/%"`` at the top of the
leading ``where`` is a semi-join of the events with the row set. Before the
main scan the View reads the row set (from the index, or by running it as a
lookup side when the index holds no rows for it), keeps the keys whose value
matches, and adds ``fhash in {keys}`` to the scan filter. The pruner and the
per-event evaluator downstream see an ordinary key-valued ``in`` list and need
no special case for "this field is actually a name." This is the same
key-set pushdown that any row set gets, so a user schema's row sets prune the
same way.

A key list longer than 4096 prunes nothing: probing that many values per
chunk costs more than the scan it could save. The per-event test of a long
``in`` list is a hash-set lookup, so a wide pattern costs about as much as a
scan without the index.

See also
--------

- :doc:`fused-scan` for what happens to the chunks that survive pruning.
- :doc:`architecture` for where indexing sits in the overall query path.
- :doc:`../guides/core/duql` for the predicate syntax itself.
- :doc:`../cpp_api/indexer` for the generated indexer API reference.
