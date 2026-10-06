:description: Match a symptom to its cause and fix: empty scans, missing trace files, and other common problems, each linked to the guide that explains it.

Troubleshoot common problems
=============================

.. admonition:: Goal
   :class: goal

   Match a symptom you are seeing to its cause and the fix, without reading
   through the whole guide tree. Each entry is symptom, then cause, then fix, with
   a link to the guide that covers the mechanism in depth.

A query returns nothing, "No trace files found" or "Not a gzip trace"
----------------------------------------------------------------------

**Symptom**: ``TraceViewer("traces/")`` (or ``dftracer_view -d traces/``)
collects an empty result, the CLI logs ``No trace files found in: <dir>``,
or a query fails with ``Not a gzip trace``.

**Cause**: a directory scan lists ``.pfw``, ``.jsonl`` and ``.ndjson``
files, plain or gzip; other names, ``.json`` included, are not trace
files (pass a ``.json`` trace with ``--files``). An index
reads gzip only, so a View or an ``Indexer`` fails on a plain file rather
than reading it as empty. ``dftracer_view``, ``dftracer_run`` and
``dftracer_index`` write a gzip copy of a plain file under ``split/`` and
read that. The other common cause is a typo'd or relative path resolved
from the wrong working directory.

**Fix**: compress plain files (``dftracer_pgzip``, see
:doc:`io/compression`) or read them through the CLI tools, and confirm the
path with ``ls`` before re-running.

The first query is slow, but a later one on the same data is fast
------------------------------------------------------------------------

**Symptom**: the first ``group_by``/``agg`` against a directory you have never
indexed takes noticeably longer than the identical query run again right
after.

**Cause**: this is expected, not a bug. A genuinely fresh file with no
``.dftindex`` yet triggers a one-pass bootstrap: the first aggregation query
both answers itself and builds the full index (checkpoints, bloom filters,
stored row sets) as a byproduct. Every query after that reads the index instead of
re-scanning. See :ref:`indexing-first-touch` in :doc:`core/indexing` for
exactly which query shapes trigger it.

**Fix**: nothing needed for a one-off script. For a server or batch job where
you want the first *user-facing* query to be fast, warm the index ahead of
time with ``Indexer.ensure_indexed()`` (Python) / ``Indexer::build`` (C++) /
``dftracer_index`` - see :doc:`core/indexing`.

Results look stale after re-running a trace or replacing files
-------------------------------------------------------------------

**Symptom**: you overwrote, appended to, or re-generated the trace files in a
directory, but a query against that directory still returns the old data (or
errors in a way that suggests a shape mismatch).

**Cause**: the index of a file depends on the file's mtime and size. A view
checks both when it is created and rebuilds a stale index before the first
scan. If the rebuild fails, creating the view raises an error that names the
file (``index of <file> is stale and its rebuild failed``). A stale result
usually means a view that was created before the files changed and then kept.

**Fix**: create the view again after the files change. To rebuild ahead of
time, call ``ix.ensure_indexed()`` (``force_rebuild=True`` if the files were
replaced in place). ``dftracer_view --no-auto-index`` only refuses to build a
*missing* index, so it does not stop the refresh of a stale one. See
:doc:`core/indexing`.

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

**Cause**: ``filter()`` / ``.duql()`` push a predicate down to the index only
when it is a pure predicate over field names and nothing but filters came
before it. An expression with a value op inside the comparison (``F.a + F.b``,
``F.dur.ilog2()``) has no index form, so the ``TraceViewer`` applies it as a
plan filter over the scanned rows instead.

**Fix**: split the predicate so its pushable part is a plain field predicate
and filter on that first (``viewer.filter(F.dur > 1000).filter((F.a + F.b) >
3)``); the plain part prunes chunks and the rest filters the survivors. Plain
field predicates (``F.dur > 1000``, ``F.cat.is_in([...])``,
``F.name.like("%read%")``, an arrow such as ``fhash -> files.path == "/x"``
in ``.duql()``) push down normally. See
:doc:`core/duql`.

Query predicate parses but does not filter what you expect
-----------------------------------------------------------------

**Symptom**: a predicate on ``fhash``, ``hhash``, ``cwd``, or ``exec_hash``
never matches a path or host name, even though you can see the value in the
trace viewer.

**Cause**: traces store hashes for host, file path, and command, not the
literal string. Filtering the bare field name (``F.fhash == "..."``) compares
against the hash, not the string you typed.

**Fix**: read the string through a row set of the source with an arrow, such
as ``.duql('where fhash -> files.path == "/data/a"')`` or
``hhash -> hosts.name``. The engine turns it into a key filter on the scan.
A ``resolved.fhash.path`` name is removed and fails with an error that names
the arrow. See "Read names through row sets" in :doc:`core/duql`.

See also
--------

- :doc:`choosing-an-api` for picking the right entry point before you hit one
  of these.
- :doc:`end-to-end` for the full happy-path workflow these pitfalls interrupt.
- :doc:`analysis/diagnosing-slow-queries` for a slow-but-correct query, as
  opposed to the wrong-result problems above.
