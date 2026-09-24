:description: Match a symptom to its cause and fix: empty scans, missing .pfw.gz files, and other common problems, each linked to the guide that explains it.

Troubleshoot common problems
=============================

.. admonition:: Goal
   :class: goal

   Match a symptom you are seeing to its cause and the fix, without reading
   through the whole guide tree. Each entry is symptom, then cause, then fix, with
   a link to the guide that covers the mechanism in depth.

A query returns nothing, or "No .pfw.gz files found"
-------------------------------------------------------

**Symptom**: ``TraceViewer("traces/")`` (or ``dftracer_view -d traces/``)
collects an empty result, or the CLI logs ``No .pfw.gz files found in:
<dir>``.

**Cause**: a directory scan only looks for gzip-compressed ``.pfw.gz`` files,
recursively. Plain, uncompressed ``.pfw`` files in the same tree are not
picked up - the engine's directory scanner is gzip-only by design (see
:doc:`analysis/views` and :doc:`data/dataframe`, both of which state this).
The other common cause is a typo'd or relative path resolved from the wrong
working directory.

**Fix**: compress stray ``.pfw`` files (``dftracer_pgzip``, see
:doc:`io/compression`) or point at the directory that already holds the
``.pfw.gz`` output, and confirm the path with ``ls`` before re-running. Some
CLI tools accept ``--files`` to pass explicit paths instead of a directory scan
if you need to bypass the extension filter for a one-off file.

The first query is slow, but a later one on the same data is fast
------------------------------------------------------------------------

**Symptom**: the first ``group_by``/``agg`` against a directory you have never
indexed takes noticeably longer than the identical query run again right
after.

**Cause**: this is expected, not a bug. A genuinely fresh file with no
``.dftindex`` yet triggers a one-pass bootstrap: the first aggregation query
both answers itself and builds the full index (checkpoints, bloom filters,
hash tables) as a byproduct. Every query after that reads the index instead of
re-scanning. See :ref:`indexing-first-touch` in :doc:`core/indexing` for
exactly which query shapes trigger it.

**Fix**: nothing needed for a one-off script. For a server or batch job where
you want the first *user-facing* query to be fast, warm the index ahead of
time with ``Indexer.ensure_indexed()`` / ``resolve_and_build_index`` /
``dftracer_index`` - see :doc:`core/indexing`.

Results look stale after re-running a trace or replacing files
-------------------------------------------------------------------

**Symptom**: you overwrote, appended to, or re-generated the trace files in a
directory, but a query against that directory still returns the old data (or
errors in a way that suggests a shape mismatch).

**Cause**: once a ``.dftindex`` exists for a file, a plain query trusts it
as-is - it does not re-check whether the underlying trace changed since the
index was built. The bootstrap in the entry above only fires for a clean
first touch, not for a file that already has a (now-stale) index.

**Fix**: index explicitly before querying a directory that may have been
mutated (``ix.ensure_indexed()`` with ``force_rebuild=True`` if the files
were replaced in place, not just added to). Two read paths refresh a
possibly-stale index automatically before every scan instead: the
``dftracer_view`` CLI (disable with ``--no-auto-index``) and the
sharded/distributed read path. See :doc:`core/indexing`.

The process gets killed, or memory climbs during a large group-by
------------------------------------------------------------------------

**Symptom**: a wide ``group_by`` over a large trace directory grows resident
memory until the OS kills the process, or a shared/HPC node's cgroup limit
kills it first.

**Cause**: an unbounded in-memory group map has no ceiling by default, and a
wide thread count multiplies that: several large group maps can build in
parallel and exhaust memory faster than a single-threaded run would. See
:doc:`runtime/performance` for how thread count and memory are independent
knobs.

**Fix**: cap and spill with ``View::memory_budget(bytes)`` / ``auto_spill()``
(same on the Python ``TraceViewer``), and check a footprint ahead of time with
``memory_budget_advice`` before committing to a run. Full detail in
:doc:`runtime/memory-budget`. If the advice says the peak does not fit on one
node, spread it with the MPI tools (:doc:`scale/mpi`).

A non-pushable predicate is not pushed to the index
-----------------------------------------------------------------------------

**Symptom**: ``viewer.filter((F.a + F.b) > 3)`` (or any predicate that mixes
arithmetic or the numeric primitives into the comparison) returns the right
rows but reads every chunk, with no index pruning.

**Cause**: ``filter()`` / ``.query()`` push a predicate down to the index only
when it is a pure predicate over field names and nothing but filters came
before it. An expression with a value op inside the comparison (``F.a + F.b``,
``F.dur.ilog2()``) has no index form, so the ``TraceViewer`` applies it as a
plan filter over the scanned rows instead.

**Fix**: split the predicate so its pushable part is a plain field predicate
and filter on that first (``viewer.filter(F.dur > 1000).filter((F.a + F.b) >
3)``); the plain part prunes chunks and the rest filters the survivors. Plain
field predicates (``F.dur > 1000``, ``F.cat.is_in([...])``,
``F.name.like("%read%")``, ``resolved(...)``) push down normally. See
:doc:`core/query-dsl`.

Query predicate parses but does not filter what you expect
-----------------------------------------------------------------

**Symptom**: a predicate on ``fpath``, ``hostname``, ``cwd``, or ``exec``
never matches anything, even though you can see the value in the trace
viewer.

**Cause**: traces store hashes for host, file path, and command, not the
literal string. Filtering the bare field name (``F.fpath == "..."``) compares
against the hash, not the string you typed.

**Fix**: use the resolved (virtual) field form - ``resolved("fpath")`` in
both Python and C++ - which the engine rewrites into a hash lookup against
the index automatically. See "Resolved (virtual) fields" in
:doc:`core/query-dsl`.

See also
--------

- :doc:`choosing-an-api` for picking the right entry point before you hit one
  of these.
- :doc:`end-to-end` for the full happy-path workflow these pitfalls interrupt.
- :doc:`analysis/diagnosing-slow-queries` for a slow-but-correct query, as
  opposed to the wrong-result problems above.
