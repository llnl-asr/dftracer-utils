:description: Author a DFTracer plugin in Python: an @jit.plugin class is AST-compiled to a native plugin that runs at native speed inside the fused scan.

JIT Plugins
===========

.. seealso::

   :doc:`api/jit` for the JIT module API reference, :doc:`tutorials/extending-the-engine`
   for a guided path, and :doc:`concepts/jit-design` for the reasons behind the rules.

The JIT layer lets you author a plugin in Python and get a native one. A class
decorated with ``@jit.plugin`` is AST-compiled to a C plugin against the stable
:doc:`plugins` ABI, built to a cached shared library, and loaded through
:class:`~dftracer.utils.plugins.Plugins` exactly like a hand-written plugin -
so a Python-authored plugin runs at native speed inside the same fused scan.

Overview
--------

- **Declare state, write one method**: a plugin is a class with scan state
  (plain annotated fields or ``jit.map`` attributes) and one ``@jit.each_event``
  method (what to do per event) or one ``@jit.each_batch`` method (what to do per
  batch of columns). Optional ``@jit.finalize`` and ``@jit.task`` methods run
  after the scan.
- **Typed keys and rich values**: a map's key is a tuple of typed fields
  (``jit.i64``, ``jit.str_``, ``jit.bytes``) and its value is an aggregate. The
  vocabulary is broad: ``count``, ``sum``, ``min`` / ``max``, ``mean``,
  ``variance`` / ``stddev``, ``quantiles`` (DDSketch percentiles), ``argmin`` /
  ``argmax``, ``topk`` / ``bottomk`` / ``approx_topk``, ``sample``,
  ``distinct``, ``set``, ``list``, and ``record`` (several aggregates on one
  key). A ``jit.map`` lowers to a host aggregation accumulator keyed by its key
  columns - the same ``DFTU_SVC_AGG`` surface a C plugin uses.
- **Statically checked**: a body is a deliberately small subset (conditions,
  locals, exceptions, files and JSON values, then map or state updates); anything
  outside it raises ``JitError`` at decoration time, never a silent miscompile, and
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

Example: a per-file event counter
---------------------------------

Every example on this page is a file in ``examples/jit/`` that a test runs, and
each starts with ``from dftracer.utils import jit``. This one is the Python
equivalent of the C plugin in :doc:`plugins`: it counts, per
``(pid, event-name)``, how many events referenced a file. Declare the map, write
the one per-event method:

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :pyobject: NameEdges

The event ``e`` exposes the parsed fields (``pid``, ``tid``, ``name``, ``cat``,
``ts``, ``dur``, ``fhash``, ``hhash``, and args when declared); ``jit.NONE`` is
the absent-field sentinel. ``jit.map`` infers ``Map[tuple[int, str], int]`` so a
type checker validates the subscript and the ``+= 1``.

Running it
----------

There is no build step to run yourself - passing the class to
:class:`~dftracer.utils.plugins.Plugins` (``from dftracer.utils.plugins import
Plugins``) compiles it to a cached native ``.so``
and loads it, exactly like a hand-written plugin. Each map comes back keyed by
its attribute name in the run's results:

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :start-at: plugins = Plugins([NameEdges])
   :end-at: df = run.results["edges"]
   :dedent: 4

Working with results (NumPy / pandas)
-------------------------------------

The per-event fold runs in compiled C, not Python - there is no NumPy in the hot
loop, which is the point. The interop happens on the *output*: each map comes
back as a small :class:`~dftracer.utils.DataFrame` that crosses to NumPy,
pandas, or Arrow cheaply (zero-copy where the dtype allows).

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :start-at: tbl = run.results["n"]
   :end-at: total = int(values.sum())
   :dedent: 4

So the pattern is: aggregate in the fused native scan, then do the NumPy / pandas
analysis on the (much smaller) result tables. If you instead want vectorized math
over *raw* events, that is the columnar :doc:`DataFrame / View <columnar-engine>`
path, not a plugin.

Richer aggregations
-------------------

The value is any aggregate, and a plugin can keep several maps. This one, per
category, keeps the event count, the total and mean duration, the slowest event
name, and an approximate top-k of event names - in one pass:

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :pyobject: CatStats

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

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :pyobject: Lat

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

.. literalinclude:: ../../examples/jit/ref_maps.py
   :language: python
   :pyobject: DurHist

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

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :pyobject: Ops

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

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :pyobject: Seen

``K`` is an ``int``, a ``str`` or a ``Tuple`` of those and ``V`` is an ``int``
or a ``float``. ``get(key, default)`` returns the stored value or the default;
``put(key, value)`` writes one. The methods mirror :class:`dftracer.utils.store.Namespace`,
and keys and values go through the same encoding, so Python reads and writes the
namespace as a ``Namespace[K, V]``:

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :start-at: store = Store.open
   :end-at: value = store.snapshot()
   :dedent: 8

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
it reads, the same mechanism the hand-written C++
``Host::publish_port`` / ``consume_port`` API uses (see
:doc:`guides/plugins/inter-plugin-comms`). The wire id of a port comes from the class and the attribute it is assigned to.
``jit.consume(Producer.sig)`` names the producer's attribute itself, so the
producer class is declared first, and a string name is for a hand-written
producer only. Declaring a port compiles to a name
in the plugin's ``provides`` / ``consumes``, so the host orders the fold
itself; there is no load order to get right.

A publish port is written with ``self.<port> += <expr>`` inside
``each_event``: the host sums the per-event contributions into one per-batch
total and publishes it once the batch finishes. A consume port is read as a
plain scalar, ``self.<port>``, giving the value the producer published for
the current batch (``0`` when no producer has published yet):

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :pyobject: Producer

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :pyobject: Consumer

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

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :start-at: @dataclass(frozen=True)
   :end-at: self.slow[(e.pid,)] += 1

.. literalinclude:: ../../examples/jit/ref_state.py
   :language: python
   :start-at: plugins = Plugins([Slow]
   :end-at: plugins = Plugins([Slow]
   :dedent: 4

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
   The whole body is one ``if`` with no ``else`` around its updates, and the
   plugin has no ``jit.Store`` field. The
   emitted code makes two passes over the batch (two-pass index selection):
   the first evaluates the condition and writes the index of each matching
   event, the second runs the updates over those events only. A nested ``if``
   inside is still a branch, on the matching events.

``"branch"``
   Everything else: an ``else`` or ``elif``, several guards, a statement
   outside the ``if``. Each event runs the C branches in order.

The result is the same either way; the mask lowering keeps the condition out of
the update loop, so the updates run only on the matching events.

.. _jit-scalar-udfs:

Scalar UDFs
-----------

``@dft.udf`` applies the same idea to one function: a typed scalar
``def f(x: float, n: int) -> float`` is compiled to a native scalar UDF that
expressions and duql call by name, with no GIL at run time. Its body is a
per-row subset (arithmetic, comparisons, ``and`` / ``or`` / ``not``,
``a if c else b``, ``abs``, ``min``, ``max``, ``math`` functions, ``if`` /
``elif`` / ``else``), and a body outside it is a located
``JitError``. ``@dft.udf(batch=True)`` keeps a function
as Python, called once per batch on numpy arrays. See
:doc:`guides/plugins/python-udfs`.

.. _jit-batch-hooks:

Batch hooks
-----------

``@jit.each_batch`` on a ``@jit.plugin`` class works on whole columns instead of
events. The method takes ``(self, df)``, where ``df`` is the scan batch, and
folds column reductions into scan state. Nothing runs per event: the body is
lowered to engine expressions that run fused over the batch, the same way
``@jit.series`` and the dataframe ``Expr`` do.

.. literalinclude:: ../../examples/jit/ref_hooks.py
   :language: python
   :pyobject: SlowReads

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
- ``s.isnull()``, ``s.notnull()``, ``s.isna()``, ``s.notna()`` and
  ``s.fill_null(v)`` with a number literal. All four masks count a NaN of a
  float column as missing.

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

A batch hook plugin takes no config, no ``jit.publish`` / ``jit.consume`` port and
no ``jit.Store``, and one class has one ``@jit.each_event`` or one
``@jit.each_batch``, not both. It may send, set and contribute to channels, latest
values and phases (see :ref:`jit-channels`). ``@jit.vfold``
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

A compiled body raises ``jit.Fatal("msg")``, ``jit.Cancelled()`` or
``jit.SkipRest()``. ``Fatal`` and ``Cancelled`` are lowered to a status of the
plugin ABI with the message copied (``dftu_fail``), and the caller of
``Plugins.run`` gets the matching Python exception, with ``message``, ``file``,
``line`` and ``col`` of the ``raise``; ``str(error)`` is
``<file>:<line>:<col>: <message>``. The classes, statuses and what a body can
catch are in :ref:`jit-errors`.

``jit.SkipRest()`` has no status and is not an error. It stops the rest of the
current batch of ``each_event`` (or the rest of a task): what the batch folded
so far is kept, no exception reaches the caller, and a task ends with its result
so far (0 unless it returned). It cannot stop a whole file, because ``on_batch``
is not told where a file ends.

The message is one string literal (a missing one is the class name). A raise of
any other class, a bare ``raise`` and ``raise ... from ...`` are compile errors
that point at the line.

A call that can fail raises a ``jit.Error`` of its own: a store get (the stored
value has the other type), a file (``jit.open``, ``for line in f``) and a JSON or
YAML parse. ``except`` catches it:

.. literalinclude:: ../../examples/jit/ref_flow.py
   :language: python
   :pyobject: Totals

A
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

.. literalinclude:: ../../examples/jit/ref_flow.py
   :language: python
   :pyobject: Fan

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

A channel send and a channel receive are in the set too (see
:ref:`jit-channels`). ``await`` is a statement of its
own, or the whole right side of ``x = await ...`` or ``a, b = await ...``; inside a
condition, an expression, a ``for`` body or a ``with`` body it is a compile error.
A task call that is not awaited is one too.

A task that is cancelled (by ``any``, or by the first failure of ``all``) stops at
its ``await`` and never runs on: no code after the
``await`` runs, and the host releases its frame exactly once. Every frame is
released once, whether its task finished, failed or was cancelled. A ``raise`` in
a task is the status of that task, and the awaiter gets it: ``Fatal`` and
``Cancelled`` end the run, and a ``jit.Error`` can be caught with ``except`` around
the ``await``.

A task is started from the ``finalize`` body or from another task, never from
``each_event`` (see :doc:`concepts/jit-design`). A plugin that has a task needs a
``finalize``.

A plugin that uses files, JSON, tasks or channels exports ``dftu_jit_stat(k)``, which counts
the task frames started (0), released (1), files opened (2) and closed (3) and
task frames completed (4), strings interned (5) and task frames cancelled (6); the
tests read it to check that every open is closed and every frame released once.

.. _jit-channels:

Channels, latest values and phase results
-----------------------------------------

A plugin talks to other plugins, and to its own tasks, through ports of the
``DFTU_SVC_PORTS`` service. A port is a class attribute: the annotation gives the
element type, ``int`` or ``float`` (a ``str`` is a view or an interned id that
lives only in the body that read it, so ``jit.Sender[str]`` is a compile error),
and the value says what the port is.

.. literalinclude:: ../../examples/jit/ref_flow.py
   :language: python
   :pyobject: Source

.. literalinclude:: ../../examples/jit/ref_flow.py
   :language: python
   :pyobject: Sink

The plugin factory declares each end to the host, with the plugin's identity as
its name (the host prefixes a port with ``plugin.<plugin>.``), and the host checks
the whole plan before the scan: a receiver whose sender is not in the set, ends
that differ in credits, policy or size, a cycle and two plugins of one name are
refused. The host's message reaches ``Plugins(...)`` as a ``JitError`` located at
the port that it names. A ``jit.receiver(Source.out)`` takes its options and its
element type from the sender it names, so the two cannot disagree: a different
element type is a compile error at the receiver. The string form,
``jit.receiver("plugin.owner.name", credits=..., policy=...)``, names a channel of
a hand-written plugin in full and must repeat the owner's options. Every
receiver, the string form included, lists its channel in ``consumes``, so the host
runs the sender's plugin first whatever the load order; a hand-written sender
lists the same full name in its ``provides``, or the host refuses the plan
(``consumes ... which no loaded plugin provides``) and does not wait for ever. A
receiver of a channel of its own plugin is a compile error (use ``jit.channel``).
A string receiver of a ``jit`` sender in the same ``Plugins(...)`` is checked
against that sender's element type; the wire is 8 raw bytes, an ``int64`` or a
``double``, and a hand-written plugin on the other end is not checked: the type is
the author's to keep.

Channels
~~~~~~~~

- ``jit.sender(credits=16, policy="fail")``, ``jit.receiver(...)`` and
  ``jit.channel(...)`` (both ends in one plugin, such as a producer task and a
  consumer task of one finalize). ``credits`` is the number of messages the channel
  holds; ``policy`` is what a send does when it is full: ``"fail"`` reports it,
  ``"drop_oldest"`` drops the oldest message and ``"block"`` waits for a credit.
  ``"block"`` is for ``jit.channel`` only: the finalize bodies of two plugins run one
  after the other, so a send that waits for another plugin could never be
  drained. A value takes 8 bytes, so ``max_msg_bytes`` is for a string-named channel
  of a hand-written plugin.
- ``self.out.send(v)`` stores the value now, and raises ``jit.Error`` (the message
  names the channel) when the channel is full or closed. ``await self.out.send(v)`` in an
  ``async`` finalize or task waits for a credit on a ``"block"`` channel.
  ``ok = self.out.try_send(v)`` never waits and says whether the value was
  stored. Both are calls of an ``each_event`` body too, except on a Block channel:
  a full channel would stop the scan loop that has to drain it, so a Block channel is
  used from an async finalize or task only.
- ``send``, ``try_send``, ``set`` and ``contribute`` in an ``each_event`` body are
  one host call for each event (a lock for each call); use an ``each_batch`` body to
  call once for each batch. A per-event ``send`` on a ``"fail"`` channel needs a credit
  for every value sent before the consumer drains it in its finalize, so give the
  channel at least that many ``credits`` or it raises ``jit.Error`` when full
  (``try_send`` returns ``False`` instead).
- ``v = await self.inp.recv()`` waits for the next value. Once the channel is
  closed and drained it raises ``jit.Closed`` (a ``jit.NotFound``), which a body can
  catch with ``except jit.Closed``. ``async for v in self.inp:`` takes values until
  then and ends without raising; its body may ``await``. A cancelled task stops at the
  receive as it does at any ``await``, and its frame is released once.
- ``self.out.close()`` (finalize or task) ends the sends: a receiver still gets what
  is queued, then the end of the channel. A plugin closes its senders when its
  finalize ends, and the host orders a sender's plugin before its receivers' (the
  sender declares the channel in its ``provides``, the receiver in its
  ``consumes``), so a consumer that loops until the end is not started before the
  sender can close.
- An ``each_batch`` body sends, sets and contributes a reduction of the batch:
  ``self.out.send(df["dur"].sum())``, once for each batch. A value is read back in a
  finalize or task.

A message of the wrong size (a C plugin that sent something else) raises
``jit.Error`` and is taken off the channel, as ``channel_recv_exact`` does.

Latest values
~~~~~~~~~~~~~

``cfg: jit.Latest[int] = jit.latest()`` is a hint: ``self.cfg.set(v)`` stores the
last value and ``x = self.cfg.get(default)`` reads it without waiting, giving
``default`` before the first set. ``jit.latest(Other.cfg)`` sets and reads another
plugin's. Which worker set last is not defined; use it for a value of which any
recent one will do.

Phase results
~~~~~~~~~~~~~

``total: jit.Phase[int] = jit.phase(merge="sum")`` (``"min"`` and ``"max"`` too)
merges the contributions of every worker: ``self.total.contribute(v)`` in
``each_event`` (or a batch body), then ``self.total.seal()`` and
``x = self.total.result(default)`` in the finalize body, which runs after the
scan. ``result`` of a phase that is still open and ``contribute`` after the seal
raise ``jit.Error``; a phase nobody contributed to reads as ``default``.
``jit.phase(Other.total)`` contributes to and reads another plugin's.

.. literalinclude:: ../../examples/jit/ref_flow.py
   :language: python
   :pyobject: Tally

A call on a port is a statement of its own or the whole right side of an
assignment (``ok = self.out.try_send(v)``, ``x = self.cfg.get(0)``,
``x = self.total.result(0)``, ``x = await self.inp.recv()``); inside an expression
or a condition it is a compile error. Misuse is a compile error with the
location: a send on a receiver, ``recv`` or ``close`` in the per-event body,
``seal`` or ``result`` while the scan runs, a float into an ``int`` port, a missing
annotation, an unsupported element type.

.. _jit-errors:

Errors
------

Compile errors
~~~~~~~~~~~~~~

Every compile error is a ``JitError`` with one stable form: the position of the
offending node, the reason, then the source line.

.. code-block:: text

   plugins/slow.py:14:13: unsupported in @jit.each_event: for loop; use a raw C++ plugin
               for x in events:

``<file>:<line>:<col>:`` is where the JIT stopped, with 1-based line and column.
``JitError`` also carries them as ``file``, ``line``, ``col``, ``source`` and
``reason``. A construct in an ``each_event``, ``each_batch``, ``finalize`` or
``task`` body points at the node inside it; a class-level mistake points at the
class or at the attribute; a bad ``jit.map(...)`` or ``jit.topk(...)`` call points
at the call in your code. ``@jit.op`` bodies raise :exc:`~dftracer.utils.jit.JitOpError`, a subclass of
``JitError``, with the same position form. ``Plugins(...)`` raises ``JitError`` too
for a config that does not fit its dataclass and for a port plan that the host
refuses, located at the field or port.

Run-time errors
~~~~~~~~~~~~~~~

A failure of a compiled body reaches the caller of ``Plugins.run`` as one of these,
each with ``message``, ``file``, ``line`` and ``col``:

.. list-table::
   :header-rows: 1
   :widths: 22 24 54

   * - Class
     - Status
     - Raised by
   * - ``jit.Fatal``
     - ``DFTU_E_UNKNOWN``
     - ``raise jit.Fatal("msg")``. Fails the run. A body cannot catch it.
   * - ``jit.Cancelled``
     - ``DFTU_E_CANCELLED``
     - ``raise jit.Cancelled()``. Ends the run as cancelled. A ``BaseException``,
       so ``except Exception`` does not swallow it. A body cannot catch it.
   * - ``jit.Error``
     - any failed call
     - A store get of the other type, a failed file or parse call, a full or
       closed channel, a phase used out of order. ``except jit.Error`` catches it.
   * - ``jit.NotFound``
     - ``DFTU_E_NOT_FOUND``
     - A missing file or key. A ``jit.Error``.
   * - ``jit.IoError``
     - ``DFTU_E_IO``
     - A failed read or write. A ``jit.Error``.
   * - ``jit.ParseError``
     - ``DFTU_E_PARSE``
     - A JSON or YAML text that does not parse. A ``jit.Error``.
   * - ``jit.Closed``
     - ``DFTU_E_NOT_FOUND``
     - A receive on a channel that is closed and drained. A ``jit.NotFound``.

A message of a failed call has the form
``<what> <path or name>: <host text>``, and ``str(error)`` is
``<file>:<line>:<col>: <message>``.

.. _jit-lowering:

What lowers where
-----------------

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Source form
     - Lowers to
   * - ``@jit.each_event`` body
     - A C loop in ``on_batch``, one iteration for each event: ``"mask"`` or
       ``"branch"`` (see :ref:`jit-conditions`).
   * - ``@jit.each_batch`` body
     - Engine expressions fused over the batch, folded once for each batch.
   * - Plain state fields, ``jit.map``
     - Host accumulators of the ``DFTU_SVC_AGG`` service, merged by name across
       workers.
   * - ``jit.Store``
     - The plugin store service (``dftu.svc.store``), one namespace for each field.
   * - ``jit.publish``, ``jit.consume``, ``jit.sender``, ``jit.receiver``,
       ``jit.channel``, ``jit.latest``, ``jit.phase``
     - The ``DFTU_SVC_PORTS`` service, with the names in ``provides`` and
       ``consumes``.
   * - ``jit.open``, ``for line in f``, ``f.read(n)``
     - The file service (``dftu.svc.fileio``).
   * - ``jit.json``, ``jit.yaml``
     - The parsers of the host, with the value as a handle that the body frees.
   * - ``@jit.finalize``, ``@jit.task``, ``await``
     - A ``dftu_step_fn`` state machine of the task service, with one frame for
       each task.
   * - ``raise``, ``try``
     - Plugin ABI statuses (``dftu_fail``) and a status check after each call.
   * - ``jit.ilog2`` and the other primitives
     - The ``dftu_*`` helpers of ``prims.h``.
   * - ``@dft.udf``
     - A native scalar UDF, or with ``batch=True`` a Python function called on one
       lane (see :ref:`jit-scalar-udfs`).
   * - ``@jit.op``, ``@jit.series``
     - A compose op or an engine expression (see :doc:`guides/plugins/compose-ops`).

Which form each body takes:

.. list-table::
   :header-rows: 1
   :widths: 34 16 16 16 18

   * - Form
     - ``each_event``
     - ``each_batch``
     - ``finalize``
     - ``task``
   * - Locals
     - yes
     - column names only
     - yes
     - yes
   * - ``raise``, ``try``
     - yes
     - no
     - yes
     - yes
   * - ``with jit.open``, ``jit.json``
     - yes, once for each event
     - no
     - yes
     - yes
   * - ``await``
     - no
     - no
     - yes
     - yes
   * - ``send``, ``try_send``, ``set``, ``contribute``
     - yes
     - a batch reduction
     - yes
     - yes
   * - ``recv``, ``close``, ``seal``, ``result``
     - no
     - no
     - yes
     - yes

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

Index summaries: ``jit.summary``
--------------------------------

``@jit.summary`` compiles a Python class to a native index extension, a summary
that a build stores per chunk and a query reads to skip chunks. The class
declares its state as annotated ``int`` or ``float`` fields with a default (the
state of an empty chunk), and three methods:

``add(self, value)``
   Runs for each non-null, non-NaN value of each path in a chunk. Values are
   widened to a double, which is exact up to 2**53: a chunk that holds an
   integer with a magnitude above 2**53 (a nanosecond timestamp, say) stores
   nothing and is never skipped, and a filter on an integer literal above 2**53
   has no evidence.

``merge(self, other)``
   Folds the state of another chunk into ``self``. The build merges the chunks
   into the file state, which a query uses to rule out the whole file.

``may_contain(self, cond)``
   Takes a ``jit.Cond`` (``cond.op`` and ``cond.value``) and returns ``False`` only when the state proves that no value in the chunk
   satisfies the filter leaf ``path <op> cond.value``. ``cond.op`` is one of
   ``jit.EQ``, ``jit.LT``, ``jit.LE``, ``jit.GT`` and ``jit.GE``; a leaf with
   another operator, an ``any()`` leaf, a non-numeric literal or a path the
   summary does not take has no evidence and never reaches it. Soundness is
   yours: a wrong ``False`` drops matching rows.

.. code-block:: python

   import dftracer.utils as dft
   from dftracer.utils import jit

   @jit.summary(name="acme.size_range", paths=["args.size"])
   class SizeRange:
       n: int = 0
       lo: float = 0.0
       hi: float = 0.0

       def add(self, value):
           if self.n == 0 or value < self.lo:
               self.lo = value
           if self.n == 0 or value > self.hi:
               self.hi = value
           self.n += 1

       def merge(self, other):
           if other.n > 0:
               if self.n == 0 or other.lo < self.lo:
                   self.lo = other.lo
               if self.n == 0 or other.hi > self.hi:
                   self.hi = other.hi
               self.n += other.n

       def may_contain(self, cond):
           if self.n == 0:
               return False
           if cond.op == jit.EQ:
               return self.lo <= cond.value and cond.value <= self.hi
           if cond.op == jit.GE:
               return self.hi >= cond.value
           return True

   so = jit.build(SizeRange)
   with dft.plugins.load_plugin(so):
       ix = dft.index.Indexer("traces/", summaries={"size": ["acme.size_range"]})
       ix.ensure()
       print(ix.explain_text("size >= 1000"))
       # traces/a.pfw.gz: read 2 of 12 chunks
       #   summary size acme.size_range: skipped 10 of 12 chunks

The body is a small subset: ``self`` fields, locals, numbers and booleans,
``+ - * /``, comparisons (chained too), ``and``, ``or``, ``not``, conditional
expressions, ``min``, ``max`` and ``abs``, ``if`` and ``elif`` and ``else``,
assignments and augmented assignments, and ``return`` in ``may_contain``. A
construct outside it raises ``JitError`` with its line and column. The built
plugin is plain C that links no Python: after the build, a query runs
the summary on native threads and no Python code runs, and it still prunes when
the class has been deleted. A chunk whose path holds a string, or several
types, stores nothing for the summary and is never skipped. The build hashes the
compiled body into the extension's params hash, so a changed body rebuilds the
data, and ``version`` rebuilds it by hand. A path in ``summaries`` that lists
only registered summaries keeps no built-in summary; list ``"zonemap"``,
``"bloom"`` or ``"counts"`` next to it to keep one.
