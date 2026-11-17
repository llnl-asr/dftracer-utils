:description: Author a DFTracer plugin in Python: an @jit.plugin class is AST-compiled to a native plugin that runs at native speed inside the fused scan.

JIT Plugins
===========

.. seealso::

   :doc:`api/jit` for the JIT module API reference.

The JIT layer lets you author a plugin in Python and get a native one. A class
decorated with ``@jit.plugin`` is AST-compiled to a C plugin against the stable
:doc:`plugins` ABI, built to a cached shared library, and loaded through
:class:`~dftracer.utils.plugins.Plugins` exactly like a hand-written plugin -
so a Python-authored plugin runs at native speed inside the same fused scan.

Overview
--------

- **Declare maps, write one method**: a plugin is a class with ``jit.map``
  attributes (the aggregations it maintains) and a single ``@jit.each_event``
  method (what to do per event).
- **Typed keys and rich values**: a map's key is a tuple of typed fields
  (``jit.i64``, ``jit.str_``, ``jit.bytes``) and its value is an aggregate. The
  vocabulary is broad: ``count``, ``sum``, ``min`` / ``max``, ``mean``,
  ``variance`` / ``stddev``, ``quantiles`` (DDSketch percentiles), ``argmin`` /
  ``argmax``, ``topk`` / ``bottomk`` / ``approx_topk``, ``sample``,
  ``distinct``, ``set``, ``list``, and ``record`` (several aggregates on one
  key). A ``jit.map`` lowers to a host aggregation accumulator keyed by its key
  columns - the same ``DFTU_SVC_AGG`` surface a C plugin uses.
- **Statically checked**: the structured ``each_event`` body is a deliberately
  small subset (an optional ``if`` guard around one map update); anything outside
  it raises ``JitError`` at decoration time - never a silent miscompile - and
  ``jit.map`` infers the ``Map[tuple[K], V]`` type so a type checker validates
  your keys and values.
- **Compiled once, computed once**: a subexpression used by more than one map in
  the same block - a hash key like ``jit.fastrange(jit.mix64(e.hhash), 256)``
  fed to several maps - is hoisted to a local and evaluated a single time per
  event, never re-run per map. The hoist stays inside
  its ``if`` guard, so a guarded expression keeps its execution condition.
- **One lookup per key**: several plain maps always subscripted at the same key
  (``self.n[(e.pid,)] += 1``; ``self.tot[(e.pid,)] += e.dur``; ...) are coalesced
  into one internal table, so the per-event hash lookup happens once for all of
  them instead of once per map. They still come back as the separate tables you
  declared - the coalescing is invisible, like the CSE above.

Tutorial: a per-file event counter
-----------------------------------

This is the Python equivalent of the C plugin in :doc:`plugins` - it counts,
per ``(pid, event-name)``, how many events referenced a file. Declare the map,
write the one per-event method:

.. code-block:: python

   from dftracer.utils import jit

   @jit.plugin
   class NameEdges:
       edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

       @jit.each_event
       def step(self, e):
           if e.fhash != jit.NONE:            # only file-referencing events
               self.edges[(e.pid, e.name)] += 1

The event ``e`` exposes the parsed fields (``pid``, ``tid``, ``name``, ``cat``,
``ts``, ``dur``, ``fhash``, ``hhash``, and args when declared); ``jit.NONE`` is
the absent-field sentinel. ``jit.map`` infers ``Map[tuple[int, str], int]`` so a
type checker validates the subscript and the ``+= 1``.

Running it
----------

There is no build step to run yourself - passing the class to
:class:`~dftracer.utils.plugins.Plugins` compiles it to a cached native ``.so``
and loads it, exactly like a hand-written plugin. Each map comes back keyed by
its attribute name in the run's results:

.. code-block:: python

   from dftracer.utils.plugins import Plugins

   plugins = Plugins([NameEdges])        # compiles + caches the .so, then loads
   run = plugins.run("./traces")
   df = run.results["edges"].to_pandas()  # columns: pid, name, value

Working with results (NumPy / pandas)
-------------------------------------

The per-event fold runs in compiled C, not Python - there is no NumPy in the hot
loop, which is the point. The interop happens on the *output*: each map comes
back as a small :class:`~dftracer.utils.DataFrame` that crosses to NumPy,
pandas, or Arrow cheaply (zero-copy where the dtype allows).

.. code-block:: python

   tbl = run.results["edges"]                    # DataFrame
   values = tbl.to_arrow().column("value").to_numpy(zero_copy_only=False)
   df = tbl.to_pandas()                          # pandas.DataFrame

   # from here it is ordinary NumPy / pandas
   top = df.sort_values("value", ascending=False).head(10)
   total = int(values.sum())

So the pattern is: aggregate in the fused native scan, then do the NumPy / pandas
analysis on the (much smaller) result tables. If you instead want vectorized math
over *raw* events, that is the columnar :doc:`DataFrame / View <columnar-engine>`
path, not a plugin.

Richer aggregations
-------------------

The value is any aggregate, and a plugin can keep several maps. This one, per
category, keeps the event count, the total and mean duration, the slowest event
name, and an approximate top-k of event names - in one pass:

.. code-block:: python

   @jit.plugin
   class CatStats:
       n      = jit.map(key=(jit.str_,), value=jit.count())
       total  = jit.map(key=(jit.str_,), value=jit.sum())
       avg    = jit.map(key=(jit.str_,), value=jit.mean())
       slow   = jit.map(key=(jit.str_,), value=jit.max())
       worst  = jit.map(key=(jit.str_,), value=jit.argmax(of=jit.str_))
       hot    = jit.map(key=(jit.str_,), value=jit.approx_topk(16))

       @jit.each_event
       def step(self, e):
           self.n[(e.cat,)]     += 1               # additive aggregates use +=
           self.total[(e.cat,)] += e.dur           # (an arithmetic expr is fine)
           self.avg[(e.cat,)].observe(e.dur)       # others observe a value
           self.slow[(e.cat,)].observe(e.dur)
           self.worst[(e.cat,)].observe(e.name, by=e.dur)   # name at the max dur
           self.hot[(e.cat,)].observe(e.name)               # frequent event names

Two contribution shapes cover the vocabulary: additive aggregates (``count``,
``sum``) take ``+= <expr>``, and every other aggregate takes ``.observe(x)`` -
with a ``by=`` keyword for the argument-style aggregates (``argmin`` /
``argmax`` / ``topk`` / ``bottomk``) that keep a payload at the extreme of a
score. A single key can carry several aggregates at once with a ``record``:
``value=dict(n=jit.count(), dur=jit.sum())`` then ``self.m[(k,)].n += 1`` /
``self.m[(k,)].dur += e.dur``.

Percentiles
-----------

``jit.quantiles`` keeps a mergeable DDSketch per key, so p50/p90/p99-style
latency percentiles come out of the one pass without collecting the values.
Pass the quantiles you want; each becomes a result column:

.. code-block:: python

   @jit.plugin
   class Lat:
       lat = jit.map(key=(jit.str_,), value=jit.quantiles(qs=(0.5, 0.9, 0.99)))

       @jit.each_event
       def step(self, e):
           self.lat[(e.cat,)].observe(e.dur)

The map materializes to a ``count`` column plus one f64 column per quantile,
named ``p50``, ``p90``, ``p99`` (a non-integer like ``0.999`` becomes
``p99_9``). The estimates are approximate - DDSketch holds ~1% relative error -
which is what makes them mergeable and O(1) in memory.

Numeric primitives
------------------

A key or value expression can call a small set of numeric primitives - the
operations that are non-obvious or slow to spell out in C by hand. Each lowers
to the matching header-only helper in ``prims.h`` (``jit.ilog2`` ->
``dftu_ilog2_u64``), so there is no Python cost and no divergence from the C. A
log2 duration histogram, keyed by bucket:

.. code-block:: python

   @jit.plugin
   class DurHist:
       hist = jit.map(key=(jit.i64,), value=jit.count())

       @jit.each_event
       def step(self, e):
           self.hist[(jit.ilog2(e.dur),)] += 1     # 0..63 log-scale bucket

The vocabulary. The integer ops are 64-bit (a Python ``int`` is unbounded, but
these compile to 64-bit C and wrap there); the float ops are ``double``:

- **bit ops**: ``ilog2`` (floor log2 / top set bit), ``bit_width``, ``clz``,
  ``ctz``, ``popcount``, ``ceil_pow2``, ``floor_pow2``, ``rotl``, ``rotr``.
- **integer math**: ``abs``, ``clamp(x, lo, hi)``, ``div_ceil``, ``div_round``,
  ``align_up(x, a)``, ``align_down(x, a)``, ``isqrt``, ``gcd``.
- **fast hashing**: ``mix64`` (SplitMix64 finalizer), ``mul_hi``, and
  ``fastrange(x, n)`` (map a hash into ``[0, n)`` with no modulo) - e.g.
  ``jit.fastrange(jit.mix64(e.hhash), 256)`` to bucket ids into 256 shards.
- **float math**: ``sqrt``, ``log2`` / ``log`` / ``exp`` (unreachable from a
  structured body otherwise), ``fma(a, b, c)`` (``a*b + c``, one rounding),
  ``lerp(a, b, t)``, ``fmin`` / ``fmax``, ``clampf(x, lo, hi)``,
  ``copysign(x, y)``.

These are authoring markers: they only compile inside a ``@jit.each_event``
body, and calling one directly in Python raises ``JitError``. The same helpers
are available to hand-written and raw-body plugins as ``dftu_*`` from
``<dftracer/utils/plugins/prims.h>``.

.. _jit-plain-state:

Plain-container state
---------------------

A plugin can keep its scan state in ordinary annotated class fields instead of
``jit.map`` descriptors. The annotation gives the types, the update code gives
the merge, and the plugin lowers to the same accumulators the descriptors use, so
the host merges the slices of the workers in the same way and each field comes
back under its name as the same one-key-per-row frame.

.. code-block:: python

   from collections import defaultdict
   from typing import Dict, List, Set, Tuple

   @jit.plugin
   class Ops:
       by_name: Dict[Tuple[int, str], int] = defaultdict(int)
       bytes_by_pid: Dict[int, int] = {}
       names: Set[str] = set()
       order: List[int] = []
       total: int = 0
       secs: float = 0.0

       @jit.each_event
       def step(self, e):
           self.by_name[(e.pid, e.name)] += 1
           self.bytes_by_pid[e.pid] = self.bytes_by_pid.get(e.pid, 0) + e.arg_i64("ret")
           self.names.add(e.name)
           self.order.append(e.ts)
           self.total += 1
           self.secs += e.dur * 0.5

The field types are ``Dict[K, V]`` or ``DefaultDict[K, V]`` (``K`` is an ``int``,
a ``str`` or a ``Tuple`` of those, ``V`` an ``int`` or a ``float``),
``Set[T]`` and ``List[T]`` (``T`` an ``int`` or a ``str``), and a bare ``int`` or
``float`` for a whole-scan scalar. Builtin generics (``dict[int, int]``) work
where the running Python allows them; use ``typing.Dict`` on Python 3.8. A field
starts empty or at 0: the default is the empty container, ``defaultdict(int)``
or ``defaultdict(float)``, or absent, and any other default is a compile error,
because every worker slice starts from it. Declare each field on the plugin class
itself; a base class is not read.

The merge is inferred from the update code only. A field is accepted when every
update to it has one of these forms and all of them agree:

.. list-table::
   :header-rows: 1
   :widths: 40 20 40

   * - Update
     - Merge
     - Needs
   * - ``d[k] += x``
     - sum
     - a ``defaultdict``
   * - ``d[k] = d.get(k, 0) + x``
     - sum
     - any dict
   * - ``d[k] = max(d[k], x)``, ``min(d[k], x)``
     - max, min
     - a ``defaultdict``
   * - ``d[k] = max(d.get(k, x), x)``, ``min(d.get(k, x), x)``
     - max, min
     - any dict
   * - ``s.add(x)``
     - union
     - a ``Set``
   * - ``lst.append(x)``
     - concatenation
     - a ``List``
   * - ``n += x``
     - sum
     - an ``int`` or ``float`` field

``max(x, d[k])`` and ``d.get(k, 0)`` on either side of the ``+`` are the same
forms. Any other update to a container field is a compile error at that line:
the merge cannot be inferred. It says so and points at a future ``merge()`` hook;
no ``merge()`` is implemented yet. A field that the body never updates, and a
read of a field inside the body (a worker holds only its own slice, so a read
would see a partial value), are compile errors too.

A missing key follows Python. On a plain ``dict``, ``d[k] += x`` and
``max(d[k], x)`` raise ``KeyError`` for a missing key, so they are a compile
error; write ``d[k] = d.get(k, 0) + x`` or declare the field
``defaultdict(int)``. A ``defaultdict`` creates the key with 0 first, so
``d[k] = max(d[k], x)`` on a ``defaultdict`` keeps the 0 when every ``x`` is
negative, exactly as Python would. A guard such as ``if k in d`` is not an
accepted form: nothing else fills a plain dict, so it would never run.

A ``List`` merges by concatenation in order of the event timestamp
(``e.ts``). The host merges the worker slices in a fixed worker order, but
which events a worker sees depends on scheduling, so arrival order is not
stable; sorting by timestamp is. Events with equal timestamps sort by the text
of the value, which is deterministic, so a list is the same with one worker or
many. A ``List[int]`` and a ``List[str]`` cell, like a ``Set`` cell, comes back
as strings (the list as strings, the set joined into one string).
A ``Set`` comes
back as one string cell of the distinct values sorted and joined by ``\x1e``, as
``jit.set`` does.

Float sums (``float`` scalars and ``Dict[K, float]`` values) are not
bit-stable: the order in which worker slices add differs between worker counts
and runs, so the last bits can differ. Compare them with a tolerance; integer
sums, extrema, sets and lists are exact.

Keys and elements are event fields and expressions of the declared family: a
``str`` key or element needs a string field such as ``e.name`` or
``e.arg_str(...)``, and an ``int`` one cannot be a string field.

Plain state and ``jit.map`` descriptors can share one plugin, and the
descriptors stay. They express what the update code cannot: ``mean``,
``variance``, ``quantiles``, ``topk``, ``argmax``, ``distinct``, ``hist``,
product and record values, an ``order_by=`` of your choice, and the fixed-width
``of=`` types. Use the plain form for counts, sums, extrema, sets and lists, and
a descriptor for the rest.

.. _jit-store:

Persistent state: ``jit.Store``
-------------------------------

``jit.Store[K, V]`` is a persistent key-value field. Declare it by annotation
and read and write it with explicit keys from the body:

.. code-block:: python

   @jit.plugin
   class Seen:
       runs: jit.Store[int, int]
       first_seen = jit.map(key=(jit.i64,), value=jit.min(of=jit.i64))

       @jit.each_event
       def step(self, e):
           self.first_seen[(e.pid,)].observe(self.runs.get(e.pid, 0))
           self.runs.put(e.pid, self.runs.get(e.pid, 0) + 1)

``K`` is an ``int``, a ``str`` or a ``Tuple`` of those and ``V`` is an ``int``
or a ``float``. ``get(key, default)`` returns the stored value or the default;
``put(key, value)`` writes one. The methods mirror :class:`dftracer.utils.store.Namespace`,
and keys and values go through the same encoding, so Python reads and writes the
namespace as a ``Namespace[K, V]``:

.. code-block:: python

   from dftracer.utils.store import Store

   store = Store.open(f"{index_dir}/.dftindex/store")  # the store of that index root
   ns = store.namespace("plugin.<plugin name>.runs", 1, durable=True, shared=True)
   store.snapshot().get(ns, None, 1)

The namespace is the plugin name plus the field name: the host registers the
field ``runs`` as ``plugin.<plugin name>.runs``, so the same plugin keeps one
namespace across runs and two plugins that use the same field name keep separate
data. The plugin name is the stem of the plugin's ``.so``: for a plugin built
into the cache it is the class identity (the package, module and class, each
byte outside ``[a-z0-9_-]`` written as ``_``), so a change of the body keeps the
data; for ``build(out=...)`` it is the file name. The field name must be
lowercase ASCII ``[a-z0-9_]``. The namespace is ``durable`` and ``shared`` with
format version 1: a shared namespace keeps the keys of every writer, and the
newest write of a key wins.

Scope rules follow :doc:`plugins`. The store is the store of the index root the
scan reads and is lent only while a fold callback runs; a scan over two index
roots fails the call. The keys live in the whole-root scope. ``get`` reads from a
snapshot taken when the batch starts, so it sees what earlier runs and earlier
folds committed and not what this fold's own ``put`` calls wrote; those are
committed when the batch ends and read back in the next run. Keep totals of the
scan in plain state and use a ``Store`` to carry a value to the next run. A
stored value of the other type (a ``float`` in an ``int`` field), a missing
string key and a failed commit fail the run with an error that names the field.

A ``Series``, ``DataFrame`` or ``Expr`` is valid only inside one ``step`` call
and cannot outlive it, so a field annotated with one, or a ``Store`` whose
value is one, is a compile error; fold the batch into plain state instead.

Talking to another plugin
--------------------------

Two JIT plugins loaded in the same run can pass a batch-scoped value between
them: ``jit.publish`` declares a port this plugin writes, ``jit.consume`` one
it reads - both keyed by a port name, the same mechanism the hand-written C++
``Host::publish_port`` / ``consume_port`` API uses (see
:doc:`guides/plugins/inter-plugin-comms`). Declaring a port compiles to a name
in the plugin's ``provides`` / ``consumes``, so the host orders the fold
itself; there is no load order to get right.

A publish port is written with ``self.<port> += <expr>`` inside
``each_event``: the host sums the per-event contributions into one per-batch
total and publishes it once the batch finishes. A consume port is read as a
plain scalar, ``self.<port>``, giving the value the producer published for
the current batch (``0`` when no producer has published yet):

.. code-block:: python

   @jit.plugin
   class Producer:
       seen = jit.map(key=(jit.i64,), value=jit.count())
       sig = jit.publish("com.example.batchsig", of=jit.u64)

       @jit.each_event
       def step(self, e):
           self.seen[(e.pid,)] += 1
           self.sig += 1

   @jit.plugin
   class Consumer:
       got = jit.map(key=(jit.i64,), value=jit.count())
       sig = jit.consume("com.example.batchsig", of=jit.u64)

       @jit.each_event
       def step(self, e):
           if self.sig > 0:               # a producer published this batch
               self.got[(e.pid,)] += 1

``of`` picks the wire width shared by both ends (``jit.u64`` / ``jit.i64`` /
``jit.f64``); it must match on both the publisher and the consumer. A
consume port is read-only (writing to it raises ``JitError`` at decoration
time) and a publish port is write-only (reading it likewise raises). A missing
producer degrades to reading ``0``.

The ordering rule matters here exactly as it does in C++: a producer's
``step`` must run before the consumer's for the same batch, which the declared
``provides`` / ``consumes`` guarantee regardless of load order. A JIT plugin
can publish for a hand-written C++ consumer and vice versa; both sides go
through the same ``DFTU_SVC_PORTS`` machinery.

.. _jit-config:

Runtime configuration
---------------------

A plugin takes load-time settings through a dataclass. Annotate the class with
``config: Cfg``, where ``Cfg`` is a (frozen) dataclass, and read each field in
the body as ``self.config.<field>``:

.. code-block:: python

   from dataclasses import dataclass
   from typing import Optional

   @dataclass(frozen=True)
   class Cfg:
       threshold: int = 0         # int, float, bool and str are native values
       label: str = "read"
       cap: Optional[int] = None  # Optional of one of them may be unset

   @jit.plugin
   class Slow:
       config: Cfg
       slow = jit.map(key=(jit.i64,), value=jit.count())

       @jit.each_event
       def step(self, e):
           if e.dur > self.config.threshold and e.name == self.config.label:
               self.slow[(e.pid,)] += 1

   Plugins([Slow], config={"Slow": {"threshold": 45}})

Defaults come from the dataclass: a field the config leaves out takes its
default, and a field with no default is required. Plugins checks the values
before it loads the plugin and raises ``JitError`` that names the field when a
required field is missing, a key is not a field, or a value has the wrong type
(an ``int`` is accepted for a ``float`` field; a ``bool`` is not an ``int``). A
field of any other type, such as a list, is a compile error that points at the
field in the dataclass.

In the body a numeric field is a number and a ``bool`` field may stand alone as
a guard (``if self.config.enabled:``). A ``str`` field compares with an event
string (``e.name == self.config.label``) and may be a ``str_`` key; it cannot be
used in arithmetic. ``self.config.cap is None`` and ``is not None`` test an
``Optional`` field; read as a number, an unset field is ``0``.

Each loaded ``Plugins`` instance holds its own config, so two instances of one
plugin with different values can run side by side, one after the other or on
different threads. A ``str`` value (NUL bytes included) is copied into the
instance when it loads and freed when the instance is destroyed; the plugin
never reads the caller's string after the load. An ``Optional`` field that the
config sets to ``None`` stays unset even when the dataclass default is a value.

Compile errors
--------------

Every compile error is a ``JitError`` with one stable form: the position of the
offending node, the reason, then the source line.

.. code-block:: text

   plugins/slow.py:14:13: unsupported in @jit.each_event: for loop; use a raw C++ plugin
               for x in events:

``<file>:<line>:<col>:`` is where the JIT stopped, with 1-based line and column.
``JitError`` also carries them as ``file``, ``line``, ``col``, ``source`` and
``reason``. A construct in the ``each_event`` body or a ``vfold`` body points at
the node inside it; a class-level mistake points at the class or at the
attribute; a bad ``jit.map(...)`` or ``jit.topk(...)`` call points at the call
in your code. ``@jit.op`` bodies raise ``JitOpError``, a subclass of
``JitError``, with the same position form.

Raw bodies: the full ABI without leaving JIT
--------------------------------------------

The structured subset covers the common counting and aggregation shapes safely.
For anything it cannot express, ``@jit.each_event(raw=True)`` makes the method
return a C/C++ string that is spliced verbatim into the per-event loop - so the
per-event body has the full :doc:`plugins` ABI, while you keep the JIT
framework: the typed ``jit.map`` declarations, the aggregate vocabulary above,
and the compile / cache / load machinery. In scope inside a raw body are
``host`` (the ``dftu_plugin_host``) and ``e`` / ``b`` / ``i`` (the current event,
batch, and index).

So JIT covers everything the C ABI can: the structured subset for the common
case, a raw body for the rest - both compiled to one native plugin. A body that
steps outside the structured subset without ``raw=True`` raises ``JitError``
pointing here, never a silent miscompile.
