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
  small subset (conditions, locals, exceptions, files and JSON values, then map or
  state updates); anything outside
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

.. _jit-conditions:

Conditions in a per-event body
------------------------------

An ``@jit.each_event`` body branches with ``if`` / ``elif`` / ``else``, nested
``if`` statements and several guards in a row. A condition is built from:

- ``and``, ``or`` and ``not``, which keep Python's short-circuit order;
- comparisons ``<  <=  >  >=  ==  !=``, chained (``20 <= e.dur < 40``);
- ``in`` and ``not in`` against a literal tuple, list or set
  (``e.pid in (1, 3)``, ``e.name in {"read", "open"}``);
- ``x is None`` and ``x is not None`` on an ``Optional`` config field
  (``self.config.cap is None``) and on a string field (``e.fhash is None``,
  ``e.arg_str("k") is None``); a numeric field is never ``None``;
- a bare number or a ``bool`` config field as its own truth value
  (``if e.arg_i64("v"):``).

A string field compares only with a string, a string literal, a ``str`` config
field or ``jit.NONE``; ``e.cat == 3`` is a compile error. The operands of an
``and`` after the first, and the links of a chained comparison after the first,
run only when the earlier ones allow it, so a division behind ``x != 0 and``
never runs for a row where ``x`` is zero. ``/`` is true division on doubles, as
in Python, whatever the operand types; ``//`` is a compile error. The scan
fields ``pid``, ``tid``, ``ts`` and ``dur`` are unsigned 64-bit, so ``e.dur - 20``
wraps below zero; ``e.arg_i64`` is signed.

A key an event lacks reads as ``0`` (``e.arg_i64``, ``e.arg_f64``) or
``jit.NONE`` (``e.arg_str``, ``e.fhash``). A batch hook sees the same key as a
null instead, which sums, counts and comparisons skip, so
``e.arg_i64("w") == 0`` is true for an absent ``w`` and ``(w == 0).sum()`` is not.

A compile error names the line and column. These are errors: an assignment
expression (``:=``), a function call other than the numeric primitives,
``@jit.op`` transforms and ``e.arg_*``, a comparison between a string and a
number, ``is`` against anything but ``None``, ``in`` against anything but a
literal, and a string used as a truth value.

The compiler chooses one of two lowerings, and ``Plugin._jit_plugin.lowering``
says which:

``"mask"``
   The whole body is one ``if`` with no ``else`` around its updates. The
   emitted code makes two passes over the batch (two-pass index selection):
   the first evaluates the condition and writes the index of each matching
   event, the second runs the updates over those events only. A nested ``if``
   inside is still a branch, on the matching events.

``"branch"``
   Everything else: an ``else`` or ``elif``, several guards, a statement
   outside the ``if``. Each event runs the C branches in order.

The result is the same either way; the mask lowering keeps the condition out of
the update loop, so the updates run only on the matching events.

.. _jit-batch-hooks:

Batch hooks
-----------

``@jit.each_batch`` on a ``@jit.plugin`` class works on whole columns instead of
events. The method takes ``(self, df)``, where ``df`` is the scan batch, and
folds column reductions into scan state. Nothing runs per event: the body is
lowered to engine expressions that run fused over the batch, the same way
``@jit.series`` and the dataframe ``Expr`` do.

.. code-block:: python

   from dftracer.utils import jit

   @jit.plugin
   class SlowReads:
       secs: float = 0.0
       kept: int = 0
       worst = jit.map(key=(), value=jit.max(of=jit.i64))

       @jit.each_batch
       def step(self, df: jit.Frame) -> None:
           dur = df["dur"]
           keep = (dur > 30) & (df["pid"] != 3)
           slow = dur[keep]
           self.secs += (slow * 0.5).sum()
           self.kept += keep.sum()
           self.worst[()].observe(slow.max())

The vocabulary is closed. In a column expression:

- ``df["name"]``, a numeric column by string literal (``pid``, ``tid``,
  ``ts``, ``dur``, ``ph`` or ``args.<key>``); a string column is an error;
- number literals and a local name bound to an expression (``dur = df["dur"]``);
- ``+  -  *  /`` and unary ``-``;
- comparisons, chained or not, against a number or another column;
- ``&``, ``|`` and ``~`` on masks (write ``(a > 1) & (b < 2)``; ``and``, ``or``
  and ``not`` do not apply to a column);
- ``s[mask]``, which keeps the rows where the mask is true;
- ``s.where(mask, other)``, which keeps ``s`` where the mask is true and takes
  ``other`` (a number or a column) elsewhere;
- ``abs(s)`` or ``s.abs()``, and ``s.clip(lo, hi)`` with number literals;
- ``s.isnull()``, ``s.notnull()`` and ``s.fill_null(v)`` with a number literal.

A statement is either ``name = <column expression>`` or a fold of a reduction
into state:

- a reduction folded into the target that merges it exactly. The batch value
  is merged with the target's own merge, so only these pairs are accepted:

  ===========================  ===========================================
  reduction                    target
  ===========================  ===========================================
  ``sum``, ``count``           ``self.n += ...`` on an ``int`` or ``float``
                               field, or ``self.m[()] += ...`` on
                               ``jit.sum()`` or ``jit.count()``
  ``min``                      ``self.m[()].observe(...)`` on ``jit.min()``
  ``max``                      ``self.m[()].observe(...)`` on ``jit.max()``
  ``mean``                     ``self.m[()].observe(...)`` on ``jit.mean()``
  ===========================  ===========================================

  Any other pair is a compile error that names the line: the max of the
  batch maxima is the max, but the sum of them is not. A mean is not additive
  across batches, so it cannot be added into a field. A ``jit.mean()`` target
  observes every non-null element of the column, and the engine merges the
  mean exactly across batches and workers; to keep a total mean in a plain
  field, fold ``sum`` and ``count`` and divide after the run;
- ``mask.sum()`` counts the true rows.

Everything else is a compile error with the line and column: a call outside the
list, a keyed or container update, an ``if`` or loop, a return, and any way to
let a column outlive the call. A column stored on ``self``
(``self.saved = df["dur"]``), appended to a list, returned, or added to state
without a reduction is an error, because a column is valid only inside one
call. A float reduction into an ``int`` field is an error when the compiler
knows the column is a float; for an ``args.<key>`` column the type is known only
when the batch runs, and a float result into an ``int`` field then fails the
scan with a message naming the field.

Rules that follow from the lowering:

- Columns in one expression must be over the same rows. ``dur[dur > 3] + dur``
  is an error; filter both with the same mask.
- ``pid``, ``tid``, ``ts`` and ``dur`` are read as signed 64-bit inside an
  expression, so ``df["dur"] - 20`` is negative as in Python. A comparison
  against a non-negative literal runs on the column as stored.
- ``s.clip(lo, hi)`` with a float bound is a float column, whatever the column
  type, and an ``int`` field refuses its sum.
- ``min``, ``max`` and ``mean`` of an empty selection fold nothing, so a batch
  where the mask matches no row leaves the extremum alone.
- A column the batch lacks (an ``args.<key>`` no event has) is an all-null
  column, so its reductions fold ``0`` for ``sum`` and ``count`` and nothing for
  ``min``, ``max`` and ``mean``; the other statements run as usual.
- The result does not depend on how the scan splits batches for ``sum``,
  ``count``, ``min`` and ``max`` into a field with the matching merge; float
  sums can differ in the last digits between runs, as for ``each_event``.

A batch hook plugin takes no config, ports or ``jit.Store``, and one class has
one ``@jit.each_event`` or one ``@jit.each_batch``, not both. ``@jit.vfold``
keeps its own, narrower form for keyed column folds.

``benchmarks/jit_batch_bench.py`` times a batch hook against the per-event form
and against the same computation written by hand, as dataframe kernels and as
one fused ``dftu_expr`` program, for a light and a heavy (about 20 ops) body, on
traces of big and of small batches. Run with ``--threads 1``, the compute of the
batch hook is within 5 percent of the hand-written expression program (it is
0.7 to 0.9 times its net time over the scan on 4 000 000 events, median of 5).
With all threads the compute hides under the parallel scan and is below the
run-to-run noise.

.. _jit-locals:

Locals
------

A compiled body (``@jit.each_event``, ``@jit.finalize``, ``@jit.task``) can bind
locals. A local is an ``int``, a ``float``, a ``str`` or a JSON value (see
:ref:`jit-values`); the first assignment gives it its type and it keeps that type.
``x = <expr>`` binds, and ``x += <expr>`` (``-=``, ``*=``, ``/=``) updates an
``int`` or ``float`` local. The right side is built from the same numbers,
primitives, event fields and config fields as any expression of the body, and
unary ``-`` is allowed. A name that is not a local, ``self``, the event or
``jit`` is a compile error that names the line and column. A local lives in the
loop body of ``each_event`` and in the frame of a task, so it survives an
``await``. A string is an interned id, as an event string is: ``==`` and ``!=``
compare it with another string, and ``len(s)`` is its byte length.

A string that comes from a file line, a chunk or ``v.as_str()`` is a *view*: the
bytes that the host lends, not an interned id. ``==`` and ``!=`` against a literal,
a config field or another string, ``len`` and ``jit.emit`` read the bytes and intern
nothing. A view is interned only where an id is needed: a key or an element of a
plain field or a ``jit.map``. A view is valid in these places only, and a use
outside them is a compile error with the line and column:

- ``line`` of ``for line in f`` and ``key`` of ``for key, value in v.items()`` last
  to the end of their ``for`` loop.
- ``x = f.read(n)`` lasts until the next ``f.read`` and the end of the ``with``.
- ``v.as_str()`` and the values of ``for`` over ``v`` last until the document that
  ``v`` comes from is parsed again.

A view cannot be copied into a local that lives longer than its source (``keep =
line`` where ``keep`` came before the loop). Copy it where it is needed: into a
set, a map key or a result.

.. _jit-exceptions:

Exceptions
----------

A compiled body raises three exceptions, each lowered to a status of the plugin
ABI with the message copied (``dftu_fail``). The caller of ``Plugins.run`` gets
the matching Python exception, with ``message``, ``file``, ``line`` and ``col`` of
the ``raise``; ``str(error)`` is ``<file>:<line>:<col>: <message>``.

.. list-table::
   :header-rows: 1
   :widths: 24 28 48

   * - Raise
     - Status
     - Effect
   * - ``jit.Fatal("msg")``
     - ``DFTU_E_UNKNOWN``
     - Fails the run. The caller gets ``jit.Fatal``. A body cannot catch it.
   * - ``jit.SkipRest()``
     - none
     - Stops the rest of the current batch of ``each_event`` (or the rest of a
       task) with no error: what the batch folded so far is kept, and no
       exception reaches the caller. It cannot stop a whole file, because
       ``on_batch`` is not told where a file ends.
   * - ``jit.Cancelled()``
     - ``DFTU_E_CANCELLED``
     - Ends the run as cancelled. The caller gets ``jit.Cancelled``, a
       ``BaseException``, so ``except Exception`` does not swallow it. A body
       cannot catch it.

The message is one string literal (a missing one is the class name). A raise of
any other class, a bare ``raise`` and ``raise ... from ...`` are compile errors
that point at the line.

A call that can fail raises a ``jit.Error`` of its own: a store get (the stored
value has the other type), a file (``jit.open``, ``for line in f``) and a JSON or
YAML parse. ``except`` catches it:

.. code-block:: python

   try:
       with jit.open(self.config.path) as f:
           for line in f:
               d = jit.json(line)
               self.total += d.find("n").as_int()
   except jit.NotFound:
       self.missing += 1
   except (jit.ParseError, jit.IoError):
       self.bad += 1

``jit.NotFound`` is ``DFTU_E_NOT_FOUND``, ``jit.IoError`` is ``DFTU_E_IO`` and
``jit.ParseError`` is ``DFTU_E_PARSE``; ``jit.Error`` is any failed call. A
``jit.Fatal``, a cancel and an allocation failure always end the run, whatever
``except`` is in the way. The handlers are tried in order, and an error that no
handler names goes on out of the ``try``. These are compile errors: a bare
``except``, an ``except`` of a Python class (``ValueError``) or of ``jit.Fatal``,
``jit.SkipRest`` or ``jit.Cancelled``, ``except ... as name`` (a handler cannot
read the error), ``else`` and ``finally``. Inside a ``try`` body a store get must
be the whole right side of an assignment (``v = self.runs.get(k, 0)``), so a failed
get leaves no update half done. An error that the caller sees from a failed call
has ``message`` such as ``open /path: <host text>`` and the place of the call.

``@jit.each_batch`` bodies take no ``raise`` or ``try``: they are column
expressions.

.. _jit-files:

Files
-----

``with jit.open(path) as f:`` opens a file for reading through the file service
and closes it on every way out of the block: the end, a ``raise``, ``SkipRest``,
a caught error. ``path`` is a string literal or a ``str`` config field. One file
is read one way:

- ``for line in f`` gives the text lines without their newline, as strings. A gzip
  file is inflated; any other file is read as it is. A file whose last gzip member
  is cut short gives its complete lines.
- ``x = f.read(n)`` gives the next ``n`` raw bytes of the file (``n`` is a positive
  integer literal) and ``""`` at the end. A gzip file gives its stored, compressed
  bytes.

Reading a file in ``each_event`` runs once per event, so a plugin that reads a
side file per batch reads it for every event. Use the ``@jit.finalize`` body for a
file that is read once. A line and a chunk are views of the bytes that the host
lends, so a file of any size is read with no interned string unless the body
stores a line in a set or a map key (see :ref:`jit-locals`). A missing file raises
``jit.NotFound`` and a failed read ``jit.IoError`` or ``jit.Error``, with the
path. ``for`` over a file may add to a plain field or a ``jit.map`` many rows per
event; the buffers of the batch grow as they fill. Await is not allowed inside a
``with`` or a ``for`` body, and ``break``, ``continue`` and a loop ``else`` are
compile errors. Any ``for`` over something that is not a file or a JSON value stays
a compile error.

.. _jit-values:

JSON and YAML values
--------------------

``jit.json(text)`` and ``jit.yaml(text)`` parse a string (a line read from a file,
a literal) into a value, bound to a local: ``d = jit.json(line)``. The value is
valid in the body that parsed it and is freed when the body ends or when the local
is assigned again. A new parse into the same local keeps the old document until
the new one parsed; a failed parse leaves the local on the old document. A value
or a string view taken from a local points into its document, so using it after
the local is parsed again is a compile error, and so is parsing a local again in a
loop when a value taken from it before the loop is alive. Read what you need
first, as an ``int`` or ``float``, which stay valid.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Use
     - Meaning
   * - ``v.find("key")``
     - The member of an object. The key is a string literal or a ``str`` config
       field. A missing key, and ``find`` on a value that is not an object, give
       a value for which ``is_null()`` is true.
   * - ``v.as_int(default=0)``
     - An integer. A JSON number gives its integer part (``2.5`` is ``2``) and a
       ``bool`` gives 0 or 1. A null, a missing, a string or an object gives
       the default.
   * - ``v.as_float(default=0.0)``
     - A float; an integer converts. Otherwise the default.
   * - ``v.as_str(default="")``
     - The text of a string value, as a string. Otherwise the default.
   * - ``v.as_bool(default=False)``
     - A ``bool``, or an integer other than 0. Otherwise the default.
   * - ``v.is_null()``
     - True for JSON ``null``, an empty YAML value, a missing key, and a value
       that does not exist.
   * - ``len(v)``
     - The number of items of an array or members of an object, else 0.
   * - ``for x in v``
     - The items of an array, the member values of an object; nothing for any
       other value.
   * - ``for key, x in v.items()``
     - The members of an object, in text order; nothing for any other value.

JSON numbers are ``int`` when they fit 64 bits and ``float`` otherwise; a wider
integer is a parse error. A YAML scalar is text, so ``as_int`` of ``7`` is the
default: read it with ``as_str``. A parse error raises ``jit.ParseError``; its message is
``json: <line>:<column>: <reason>`` (or ``yaml:``) with the 1-based line and byte
column of the reader. A value is
not a number or a string, so using one in arithmetic, as a condition or as an
element is a compile error; read it with an accessor first. The ``find`` key and
the text of ``jit.json`` are checked at compile time.

.. _jit-tasks:

Finalize and tasks: ``async`` and ``await``
-------------------------------------------

``@jit.finalize`` marks the one method that runs when the scan ends, after the
workers' results are merged. It is ``def`` or ``async def`` and takes ``(self)``.
Its results leave through ``jit.emit(name, value)``, which emits the named result
of the run as bytes (an ``int`` in decimal, a ``float`` with 17 significant digits,
a ``str`` as UTF-8); ``run.results[name]`` is that ``bytes``. A name is emitted
once in the source, and ``emit`` inside a loop keeps the last value. A
``finalize`` body reads no scan state: the merged state is in the results of the
maps, not in the plugin.

``@jit.task`` marks an ``async def`` method with ``int`` and ``float`` parameters
(annotated) that returns an ``int``, a ``float`` or nothing. It lowers to a
``dftu_step_fn`` state machine of the task service: the locals of the task, the
parameters and the file or JSON handles it holds live in one frame that the host
keeps across each ``await``. The awaits are a closed set:

.. code-block:: python

   @jit.plugin
   class Fan:
       n: int = 0

       @jit.each_event
       def step(self, e):
           self.n += 1

       @jit.task
       async def fetch(self, a: int, secs: float) -> int:
           await jit.sleep(secs)
           return a * 10

       @jit.finalize
       async def done(self) -> None:
           one = await self.fetch(1, 0.1)
           x, y = await jit.all(self.fetch(2, 0.1), self.fetch(3, 0.1))
           first = await jit.any(self.fetch(4, 5.0), self.fetch(5, 0.01))
           jit.emit("sum", one + x + y + first)

- ``await jit.sleep(seconds)`` waits, then goes on.
- ``x, y = await jit.all(t1, t2, ...)`` runs the tasks at once and unpacks their
  results in order. The first failure cancels the rest and is the failure of the
  await.
- ``x = await jit.any(t1, t2, ...)`` runs the tasks at once; the first to finish
  is the result (or its failure) and the others are cancelled and awaited. The
  tasks share one return type. To race a deadline, race a task that sleeps and
  returns a value; ``jit.sleep`` itself cannot be an argument of ``all`` or ``any``.
- ``x = await self.task(args)`` starts a task and waits for it. A task may start
  tasks.
- ``x = await self.<store>.get(key, default)`` in a ``finalize`` body reads the
  store (a task cannot get or put: tasks run beside each other, so pass the value as an
  argument).

A channel receive is not in the set: channels come in a later group, and an
``await`` of it is a compile error that says so. ``await`` is a statement of its
own, or the whole right side of ``x = await ...`` or ``a, b = await ...``; inside a
condition, an expression, a ``for`` body or a ``with`` body it is a compile error.
A task call that is not awaited is one too.

A task that is cancelled (by ``any``, or by the first failure of ``all``) stops at
its ``await`` and never runs on: no code after the
``await`` runs, and the host releases its frame exactly once. Every frame is
released once, whether its task finished, failed or was cancelled. A ``raise`` in
a task is the status of that task, and the awaiter gets it: ``Fatal`` and
``Cancelled`` end the run, a ``jit.Error`` can be caught with ``except`` around
the ``await``, and ``SkipRest`` ends the task with its result so far (0 unless it
returned).

A task is started from the ``finalize`` body, or from another task, and not from
``each_event``: ``on_batch`` is synchronous, its row buffers die when it returns
and the ABI gives it no slot for a pending task, so a task started there could not
add to scan state and waiting for it would be a plain call. A plugin that has a
task needs a ``finalize``.

A plugin that uses files, JSON or tasks exports ``dftu_jit_stat(k)``, which counts
the task frames started (0), released (1), files opened (2) and closed (3) and
task frames completed (4); the tests read it to check that every open is closed and
every frame released once.

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
